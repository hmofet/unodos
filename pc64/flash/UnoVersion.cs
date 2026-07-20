/*  Dev placeholder.  build-flasher.ps1 generates build/UnoVersion.cs with the
 *  real yyyyMMdd-HHmmss build stamp and compiles THAT instead of this file;
 *  this one only exists so a bare `csc *.cs` still compiles.  The "0-dev"
 *  stamp marks such a hand-built exe and disables self-update for it (see
 *  UnoUpdate.IsDevBuild), so an update can never clobber work in progress. */
static class UnoVersion
{
    public const string Build = "0-dev";
}
