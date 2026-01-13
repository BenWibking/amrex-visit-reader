import os

script_dir = os.path.abspath(os.path.dirname(__file__))
dataset = os.path.join(script_dir, "Nyx_LyA", "plt00000")
OpenDatabase(dataset, 0, "amrex-plotfile")
AddPlot("Pseudocolor", "gasDensity")
DrawPlots()
Query("NumZones")
print("NumZones before InverseGhost", GetQueryOutputValue())
AddOperator("InverseGhostZone")
DrawPlots()
Query("NumZones")
print("NumZones after InverseGhost", GetQueryOutputValue())
DeleteAllPlots()
