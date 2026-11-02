<#
    What a host sees of a built Windows DLL, with no GL and no Resolume.

        tools/wincheck.ps1 -Dll build/Release/Knockout.dll -Name "SW Knockout" -Id KO01 `
                           -Kind effect -Params 34

    It LOADS the DLL (so a missing runtime or dependency fails here, where
    dumpbin cannot see it), finds plugMain, and asks it what Resolume asks
    first: FF_GET_INFO (the name, read back as the char[16] a host reads, the
    id and the type) and the parameters (their count and names) -- which the
    SDK answers from a prototype instance, so the constructor runs too, and
    with it the font scan of C:\Windows\Fonts. Then it reads the Font list as
    a host does, element by element, and requires Arial Black's value to be
    the one a Mac gives it: the value is what a composition saves, so a
    family picked on one must be the same family on the other.

    Adapted from isohypse's, without the worlds. CI runs it on the DLL it
    builds. The pixels are checked on a Mac (tools/verify.sh); a Windows
    runner has no GL to draw them with.
#>
param(
    [Parameter( Mandatory )] [string] $Dll,
    [Parameter( Mandatory )] [string] $Name,
    [Parameter( Mandatory )] [string] $Id,
    [Parameter( Mandatory )] [ValidateSet( 'source', 'effect' )] [string] $Kind,
    [Parameter( Mandatory )] [int] $Params
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Ffgl
{
    [DllImport( "kernel32", SetLastError = true, CharSet = CharSet.Unicode )]
    public static extern IntPtr LoadLibraryW( string path );
    [DllImport( "kernel32", SetLastError = true, CharSet = CharSet.Ansi )]
    public static extern IntPtr GetProcAddress( IntPtr module, string name );
    // FFMixed is a union of a 32-bit integer and a pointer: pointer-sized.
    [UnmanagedFunctionPointer( CallingConvention.StdCall )]
    public delegate IntPtr PlugMain( uint code, IntPtr input, IntPtr instance );
}
'@

$failures = 0
function Check( [bool] $ok, [string] $what )
{
    if( $ok ) { Write-Host "  ok    $what" }
    else { Write-Host "  FAIL  $what"; $script:failures++ }
}
# The low 32 bits of an FFMixed, as a host reads it: the high half of the
# register is whatever the plugin left there. 0xffffffffL, not 0xffffffff,
# which PowerShell reads as an Int32 -1 and so masks nothing.
function Low32( [IntPtr] $mixed ) { [uint32]( $mixed.ToInt64() -band 0xffffffffL ) }

$path = ( Resolve-Path $Dll ).Path
Write-Host "$path"
$module = [Ffgl]::LoadLibraryW( $path )
if( $module -eq [IntPtr]::Zero )
{
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "LoadLibrary failed (Win32 error $err): a missing dependency or runtime"
}
$proc = [Ffgl]::GetProcAddress( $module, 'plugMain' )
Check ( $proc -ne [IntPtr]::Zero ) 'loads, and exports plugMain'
if( $proc -eq [IntPtr]::Zero ) { exit 1 }
$plugMain = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer( $proc, [Ffgl+PlugMain] )

# FF_GET_INFO (0): PluginInfoStruct -- two FFUInt32 API versions, char[4]
# id, char[16] name (neither null-terminated), FFUInt32 type.
$info  = $plugMain.Invoke( 0, [IntPtr]::Zero, [IntPtr]::Zero )
$bytes = New-Object byte[] 32
[Runtime.InteropServices.Marshal]::Copy( $info, $bytes, 0, 32 )
$gotId   = [Text.Encoding]::ASCII.GetString( $bytes, 8, 4 )
$gotName = [Text.Encoding]::ASCII.GetString( $bytes, 12, 16 ).TrimEnd( [char]0 )
$gotType = [BitConverter]::ToUInt32( $bytes, 28 )
$wantType = if( $Kind -eq 'source' ) { 1 } else { 0 }
Check ( $gotId -eq $Id ) "id $gotId (expected $Id)"
Check ( $gotName -eq $Name ) "name '$gotName', as a host reads char[16] (expected '$Name')"
Check ( $gotType -eq $wantType ) "type $gotType, $Kind"

# FF_GET_NUM_PARAMETERS (4) and FF_GET_PARAMETER_NAME (5): the SDK builds a
# prototype instance to answer, so the plugin's constructor runs here.
$count = [int]( Low32 $plugMain.Invoke( 4, [IntPtr]::Zero, [IntPtr]::Zero ) )
$names = @()
for( $i = 0; $i -lt $count; $i++ )
{
    $p = $plugMain.Invoke( 5, [IntPtr]$i, [IntPtr]::Zero )
    $names += [Runtime.InteropServices.Marshal]::PtrToStringAnsi( $p )
}
Check ( $count -eq $Params ) "$count parameters (expected $Params): $( $names[0] ) .. $( $names[-1] )"
Check ( $names[0] -eq 'Text' -and $names[1] -eq 'Font' -and $names -contains 'About' ) 'Text and Font first; About among them'

# The Font list: FF_GET_NUM_PARAMETER_ELEMENTS (31) takes the parameter;
# FF_GET_PARAMETER_ELEMENT_NAME (35) and _VALUE (36) a pointer to two
# FFUInt32s, the parameter and the element. A value comes back as a float's
# bits in the low 32.
$font     = [array]::IndexOf( $names, 'Font' )
$families = [int]( Low32 $plugMain.Invoke( 31, [IntPtr]$font, [IntPtr]::Zero ) )
$pair     = [Runtime.InteropServices.Marshal]::AllocHGlobal( 8 )
$listed   = @{}
try
{
    for( $e = 0; $e -lt $families; $e++ )
    {
        [Runtime.InteropServices.Marshal]::WriteInt32( $pair, 0, $font )
        [Runtime.InteropServices.Marshal]::WriteInt32( $pair, 4, $e )
        $family = [Runtime.InteropServices.Marshal]::PtrToStringAnsi( $plugMain.Invoke( 35, $pair, [IntPtr]::Zero ) )
        $bits   = Low32 $plugMain.Invoke( 36, $pair, [IntPtr]::Zero )
        $listed[ $family ] = [BitConverter]::ToSingle( [BitConverter]::GetBytes( $bits ), 0 )
    }
}
finally { [Runtime.InteropServices.Marshal]::FreeHGlobal( $pair ) }
$distinct = ( $listed.Values | Sort-Object -Unique ).Count
Check ( $families -gt 1 -and $distinct -eq $families ) "the Font list: $families families from the font scan, each with its own value"
# Arena on a Mac saved Arial Black as 10146829 (2 November 2026).
$arialBlack = $listed[ 'Arial Black' ]
Check ( $arialBlack -eq 10146829 ) "Arial Black's value $arialBlack, as on a Mac (10146829)"

if( $failures -gt 0 ) { Write-Host "`n  FAIL ($failures)"; exit 1 }
Write-Host "`n  PASS"
