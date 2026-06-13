#!/usr/bin/env python3
"""Estimate memory needed to read an AMReX plotfile with this VisIt plugin."""

from __future__ import annotations

import argparse
import math
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


BOX_RE = re.compile(
    r"\(\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)\s*"
    r"\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)"
)


@dataclass
class FabBox:
  level: int
  index: int
  small: Tuple[int, int, int]
  big: Tuple[int, int, int]

  @property
  def cells(self) -> int:
    total = 1
    for lo, hi in zip(self.small, self.big):
      total *= max(0, hi - lo + 1)
    return total

  @property
  def nodes_per_axis(self) -> Tuple[int, int, int]:
    return tuple(max(1, hi - lo + 2) for lo, hi in zip(self.small, self.big))


@dataclass
class LevelInfo:
  level: int
  ncomp: int
  boxes: List[FabBox]


@dataclass
class ParticleSpecies:
  name: str
  is_single: bool
  spatial_dim: int
  num_real: int
  num_int: int
  counts: List[int]
  int_bytes: int

  @property
  def real_bytes(self) -> int:
    return 4 if self.is_single else 8

  @property
  def particles(self) -> int:
    return sum(self.counts)


@dataclass
class DomainEstimate:
  label: str
  bytes: int


def human_bytes(num_bytes: int) -> str:
  units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"]
  value = float(num_bytes)
  for unit in units:
    if abs(value) < 1024.0 or unit == units[-1]:
      return f"{value:.2f} {unit}"
    value /= 1024.0
  return f"{value:.2f} PiB"


def read_tokens(path: Path) -> List[str]:
  return path.read_text(encoding="utf-8").split()


def normalize_plotfile_path(path: Path) -> Path:
  if path.name == "Header":
    path = path.parent
  if not (path / "Header").is_file():
    raise SystemExit(f"{path} is not an AMReX plotfile directory or Header file")
  return path


def parse_plotfile_header(plotfile: Path) -> Tuple[int, List[str]]:
  tokens = read_tokens(plotfile / "Header")
  if len(tokens) < 3:
    raise SystemExit(f"{plotfile / 'Header'} is too short")
  try:
    ncomp = int(tokens[1])
  except ValueError as exc:
    raise SystemExit(f"Could not parse component count in {plotfile / 'Header'}") from exc
  var_names = tokens[2:2 + ncomp]
  if len(var_names) != ncomp:
    raise SystemExit(f"Could not parse {ncomp} variable names in {plotfile / 'Header'}")
  return ncomp, var_names


def parse_cell_header(path: Path, level: int) -> LevelInfo:
  lines = path.read_text(encoding="utf-8").splitlines()
  if len(lines) < 5:
    raise SystemExit(f"{path} is too short")
  try:
    ncomp = int(lines[2].strip())
  except ValueError as exc:
    raise SystemExit(f"Could not parse component count in {path}") from exc

  boxes: List[FabBox] = []
  for line in lines:
    match = BOX_RE.search(line)
    if match is None:
      continue
    values = tuple(int(match.group(i)) for i in range(1, 7))
    boxes.append(
      FabBox(
        level=level,
        index=len(boxes),
        small=(values[0], values[1], values[2]),
        big=(values[3], values[4], values[5]),
      )
    )
  if not boxes:
    raise SystemExit(f"No FAB boxes found in {path}")
  return LevelInfo(level=level, ncomp=ncomp, boxes=boxes)


def parse_levels(plotfile: Path) -> List[LevelInfo]:
  levels: List[LevelInfo] = []
  for child in sorted(plotfile.iterdir()):
    if not child.is_dir() or not child.name.startswith("Level_"):
      continue
    try:
      level = int(child.name.split("_", 1)[1])
    except ValueError:
      continue
    cell_header = child / "Cell_H"
    if cell_header.is_file():
      levels.append(parse_cell_header(cell_header, level))
  if not levels:
    raise SystemExit(f"No Level_*/Cell_H files found under {plotfile}")
  return sorted(levels, key=lambda item: item.level)


def infer_particle_int_bytes(
    species_dir: Path,
    level: int,
    file_num: int,
    count: int,
    offset: int,
    real_bytes_total: int,
    num_int: int,
    same_file_offsets: Sequence[int],
) -> int:
  if count <= 0 or num_int <= 0:
    return 8
  candidates = [
    species_dir / f"Level_{level}" / f"DATA_{file_num}",
    species_dir / f"Level_{level}" / f"DATA_{file_num:04d}",
  ]
  data_file = next((candidate for candidate in candidates if candidate.is_file()), None)
  if data_file is None:
    return 8
  next_offsets = [candidate for candidate in same_file_offsets if candidate > offset]
  end = min(next_offsets) if next_offsets else data_file.stat().st_size
  available = end - offset
  denom = num_int * count
  int_total = available - real_bytes_total
  if denom > 0 and int_total >= 0 and int_total % denom == 0:
    inferred = int_total // denom
    if inferred in (4, 8):
      return int(inferred)
  return 8


def parse_particle_header(species_dir: Path) -> Optional[ParticleSpecies]:
  header = species_dir / "Header"
  if not header.is_file():
    return None
  tokens = read_tokens(header)
  if len(tokens) < 3 or "Version_Two_Dot" not in tokens[0]:
    return None

  idx = 0
  version = tokens[idx]
  idx += 1
  try:
    spatial_dim = int(tokens[idx])
    idx += 1
    num_real_extra = int(tokens[idx])
    idx += 1
  except ValueError:
    return None

  idx += num_real_extra
  if idx >= len(tokens):
    return None
  try:
    num_int_extra = int(tokens[idx])
  except ValueError:
    return None
  idx += 1 + num_int_extra
  if idx + 3 >= len(tokens):
    return None
  try:
    idx += 1
    _num_particles = int(tokens[idx])
    idx += 1
    _next_id = int(tokens[idx])
    idx += 1
    finest_level = int(tokens[idx])
    idx += 1
  except ValueError:
    return None

  num_grids: List[int] = []
  for _level in range(finest_level + 1):
    if idx >= len(tokens):
      return None
    try:
      num_grids.append(int(tokens[idx]))
    except ValueError:
      return None
    idx += 1

  records: List[Tuple[int, int, int, int]] = []
  counts: List[int] = []
  for level, ngrids in enumerate(num_grids):
    for _grid in range(ngrids):
      if idx + 2 >= len(tokens):
        return None
      try:
        file_num = int(tokens[idx])
        count = int(tokens[idx + 1])
        offset = int(tokens[idx + 2])
      except ValueError:
        return None
      idx += 3
      records.append((level, file_num, count, offset))
      counts.append(count)

  is_single = "single" in version
  num_real = spatial_dim + num_real_extra
  num_int = 2 + num_int_extra
  real_bytes = 4 if is_single else 8
  inferred_int_bytes = 8
  for level, file_num, count, offset in records:
    same_file_offsets = [
      other_offset
      for other_level, other_file_num, _other_count, other_offset in records
      if other_level == level and other_file_num == file_num
    ]
    inferred_int_bytes = infer_particle_int_bytes(
      species_dir,
      level,
      file_num,
      count,
      offset,
      count * num_real * real_bytes,
      num_int,
      same_file_offsets,
    )
    if count > 0:
      break

  return ParticleSpecies(
    name=species_dir.name,
    is_single=is_single,
    spatial_dim=spatial_dim,
    num_real=num_real,
    num_int=num_int,
    counts=counts,
    int_bytes=inferred_int_bytes,
  )


def parse_particles(plotfile: Path) -> List[ParticleSpecies]:
  species: List[ParticleSpecies] = []
  for child in sorted(plotfile.iterdir()):
    if child.is_dir():
      parsed = parse_particle_header(child)
      if parsed is not None:
        species.append(parsed)
  return species


def mesh_bytes(box: FabBox, id_bytes: int, include_ids: bool) -> int:
  coords = sum(box.nodes_per_axis) * 4
  ghost_zones = box.cells
  ids = box.cells * id_bytes * 2 if include_ids else 0
  return coords + ghost_zones + ids


def field_domain_estimates(
    levels: Sequence[LevelInfo],
    real_bytes: int,
    vector_components: int,
    include_ids: bool,
    id_bytes: int,
) -> Tuple[List[DomainEstimate], List[DomainEstimate], List[DomainEstimate]]:
  scalar: List[DomainEstimate] = []
  vector: List[DomainEstimate] = []
  all_fields: List[DomainEstimate] = []
  for level in levels:
    for box in level.boxes:
      base = mesh_bytes(box, id_bytes, include_ids)
      # The plugin clears each FAB copy inside VisMF immediately after the
      # data is converted, so only one FAB component per domain is resident
      # at a time (the transient `one_read` term); the VTK output arrays are
      # what accumulate.
      one_read = box.cells * real_bytes
      one_vtk = box.cells * 8
      scalar.append(DomainEstimate(f"level {level.level} fab {box.index}", base + one_read + one_vtk))
      comps = min(vector_components, max(1, level.ncomp))
      vector.append(
        DomainEstimate(
          f"level {level.level} fab {box.index}",
          base + one_read + comps * one_vtk + comps * one_vtk,
        )
      )
      all_fields.append(
        DomainEstimate(
          f"level {level.level} fab {box.index}",
          base + one_read + level.ncomp * one_vtk,
        )
      )
  return scalar, vector, all_fields


def particle_domain_estimates(
    particles: Sequence[ParticleSpecies],
    id_bytes: int,
) -> Tuple[List[DomainEstimate], List[DomainEstimate], List[DomainEstimate]]:
  mesh: List[DomainEstimate] = []
  scalar: List[DomainEstimate] = []
  all_vars: List[DomainEstimate] = []
  for species in particles:
    for index, count in enumerate(species.counts):
      block = count * (
        species.num_real * species.real_bytes + species.num_int * 8
      )
      vtk_points = count * 3 * species.real_bytes
      vtk_verts = count * id_bytes * 2 + id_bytes
      scalar_bytes = max(species.real_bytes, species.int_bytes) * count
      all_output = (
        species.num_real * species.real_bytes * count
        + species.num_int * species.int_bytes * count
      )
      label = f"{species.name} block {index}"
      mesh.append(DomainEstimate(label, block + vtk_points + vtk_verts))
      scalar.append(DomainEstimate(label, block + scalar_bytes))
      all_vars.append(DomainEstimate(label, block + vtk_points + vtk_verts + all_output))
  return mesh, scalar, all_vars


def distribute_bytes(
    domains: Sequence[DomainEstimate],
    nodes: int,
    ranks_per_node: int,
) -> int:
  ranks = max(1, nodes * ranks_per_node)
  node_totals = [0] * nodes
  for domain_index, estimate in enumerate(domains):
    rank = domain_index % ranks
    node = min(nodes - 1, rank // ranks_per_node)
    node_totals[node] += estimate.bytes
  return max(node_totals) if node_totals else 0


def required_nodes(
    domains: Sequence[DomainEstimate],
    node_memory_bytes: int,
    ranks_per_node: int,
    safety_factor: float,
) -> int:
  if not domains:
    return 0
  usable = int(node_memory_bytes / safety_factor)
  if usable <= 0:
    return math.inf
  lower_bound = max(1, math.ceil(sum(item.bytes for item in domains) / usable))
  nodes = lower_bound
  while True:
    if distribute_bytes(domains, nodes, ranks_per_node) <= usable:
      return nodes
    nodes += 1


def summarize(
    name: str,
    domains: Sequence[DomainEstimate],
    node_memory_bytes: int,
    ranks_per_node: int,
    safety_factor: float,
) -> str:
  if not domains:
    return f"{name:24} no domains"
  total = sum(item.bytes for item in domains)
  peak = max(domains, key=lambda item: item.bytes)
  nodes = required_nodes(domains, node_memory_bytes, ranks_per_node, safety_factor)
  node_peak = distribute_bytes(domains, max(1, nodes), ranks_per_node) if nodes else 0
  return (
    f"{name:24} total={human_bytes(total):>12}  "
    f"largest-domain={human_bytes(peak.bytes):>11}  "
    f"nodes={nodes:>4}  node-peak={human_bytes(node_peak):>11}"
  )


def main(argv: Optional[Sequence[str]] = None) -> int:
  parser = argparse.ArgumentParser(
    description=(
      "Estimate memory usage for this VisIt AMReX plotfile plugin. "
      "The estimate is based on plugin allocation patterns: rectilinear mesh "
      "coordinates, ghost arrays, AMReX FAB component reads, VTK output arrays, "
      "and modern AMReX particle block reads."
    )
  )
  parser.add_argument("plotfile", type=Path, help="AMReX plotfile directory or Header")
  parser.add_argument("--node-memory-gib", type=float, default=256.0,
                      help="memory per Andes node in GiB (default: 256)")
  parser.add_argument("--ranks-per-node", type=int, default=56,
                      help="MPI ranks per Andes node (default: 56)")
  parser.add_argument("--safety-factor", type=float, default=1.25,
                      help="multiply memory estimate before sizing nodes (default: 1.25)")
  parser.add_argument("--amrex-real-bytes", type=int, choices=(4, 8), default=8,
                      help="AMReX Real size for field FAB data (default: 8)")
  parser.add_argument("--vtk-id-bytes", type=int, choices=(4, 8), default=8,
                      help="VTK id type size for ids/connectivity (default: 8)")
  parser.add_argument("--vector-components", type=int, default=3,
                      help="components in a typical field vector request (default: 3)")
  parser.add_argument("--include-global-ids", action="store_true",
                      help="include optional global node and zone id auxiliary arrays")
  args = parser.parse_args(argv)

  plotfile = normalize_plotfile_path(args.plotfile)
  header_ncomp, var_names = parse_plotfile_header(plotfile)
  levels = parse_levels(plotfile)
  particles = parse_particles(plotfile)

  scalar, vector, all_fields = field_domain_estimates(
    levels,
    real_bytes=args.amrex_real_bytes,
    vector_components=args.vector_components,
    include_ids=args.include_global_ids,
    id_bytes=args.vtk_id_bytes,
  )
  particle_mesh, particle_scalar, particle_all = particle_domain_estimates(
    particles,
    id_bytes=args.vtk_id_bytes,
  )

  node_memory_bytes = int(args.node_memory_gib * 1024 ** 3)
  field_cells = sum(box.cells for level in levels for box in level.boxes)
  field_domains = sum(len(level.boxes) for level in levels)
  level_components = ", ".join(f"L{level.level}:{level.ncomp}" for level in levels)

  print(f"plotfile: {plotfile}")
  print(f"field variables from Header: {header_ncomp} ({', '.join(var_names[:6])}{'...' if len(var_names) > 6 else ''})")
  print(f"field levels/domains/cells: {len(levels)} / {field_domains} / {field_cells}")
  print(f"Cell_H components by level: {level_components}")
  print(f"particle species: {len(particles)}")
  for species in particles:
    print(
      f"  {species.name}: particles={species.particles} blocks={len(species.counts)} "
      f"real_components={species.num_real} int_components={species.num_int} "
      f"real_bytes={species.real_bytes} int_bytes~={species.int_bytes}"
    )
  print(
    f"Andes sizing assumptions: {args.node_memory_gib:g} GiB/node, "
    f"{args.ranks_per_node} ranks/node, safety factor {args.safety_factor:g}"
  )
  print()
  print(summarize("field scalar request", scalar, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print(summarize("field vector request", vector, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print(summarize("all field components", all_fields, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print(summarize("particle mesh request", particle_mesh, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print(summarize("particle scalar request", particle_scalar, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print(summarize("all particle arrays", particle_all, node_memory_bytes, args.ranks_per_node, args.safety_factor))
  print()
  print("Notes:")
  print("  - Field variables are demand-loaded by VisIt; the plugin does not read every component unless VisIt requests them.")
  print("  - The field vector estimate includes temporary scalar component arrays plus the final vector array.")
  print("  - Particle requests reread the selected particle block; all-particle-arrays estimates retained output arrays plus one block read.")
  print("  - Increase --safety-factor if VisIt filters/operators will keep additional derived arrays resident.")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
