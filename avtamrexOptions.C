// ABOUTME: Defines the read options offered by the amrex database plugin in
// ABOUTME: VisIt's file-open options dialog.

#include <avtamrexOptions.h>

#include <DBOptionsAttributes.h>

DBOptionsAttributes *
GetamrexReadOptions(void)
{
    DBOptionsAttributes *rv = new DBOptionsAttributes;
    rv->SetBool(AMREX_OPT_DOMAIN_BOUNDARIES, true);
    rv->SetBool(AMREX_OPT_INVARIANT_MESH, false);
    return rv;
}
