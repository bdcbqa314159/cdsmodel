# C8 -- run the add-in in real Excel and report its verdict, reproducibly.
#
# An XLL only truly runs inside Excel. `tools/gen_xll_workbook.py` writes a
# workbook that reports its own PASS/FAIL per row; this drives it over COM so the
# evidence is a command, not "I opened it and it looked fine". It is the same
# Excel calculation engine and the same registered add-in -- scripting only
# removes the hand-clicking.
#
#   cmake --preset release
#   cmake --build --preset release --target cdsmodel_xll
#   python tools/gen_xll_workbook.py        # (optional) refresh tests/xll/smoke.xlsx
#   pwsh tools/run_c8.ps1                    # then paste the output into C8.md
#
# Needs 64-bit Excel installed (an x64 .xll will not load in 32-bit Excel) and
# the .xll built. Exits non-zero if the add-in fails to register.
param(
    [string]$Xll      = (Join-Path $PSScriptRoot '..\build\release\xl\interface\cdsmodel.xll'),
    [string]$Workbook = (Join-Path $PSScriptRoot '..\tests\xll\smoke.xlsx')
)
$ErrorActionPreference = 'Stop'
$Xll = (Resolve-Path $Xll).Path
$Workbook = (Resolve-Path $Workbook).Path

$app = New-Object -ComObject Excel.Application
$app.Visible = $false
$app.DisplayAlerts = $false
$exit = 0
try {
    "Excel        : $($app.Version) build $($app.Build)"
    $registered = $app.RegisterXLL($Xll)
    "RegisterXLL  : $registered"
    if (-not $registered) { throw 'the add-in did not register -- is this 64-bit Excel?' }

    # Name-by-name registration sweep. #NAME? means Excel does not know the name;
    # anything else (a value, #VALUE!, or the add-in's own error text) means it
    # registered and dispatched.
    $names = @(
        'CdsPrice','CDS.Price','LoadHolidays','CDS.Calendar','CDS.CalendarFromFile',
        'CDS.CalendarDates','IRZeroCurveMake','IRZeroCurveBuild','CDS.IRZeroCurve',
        'DiscountFactor','CleanSpreadCurveBuild','CDS.SpreadCurve','ParSpreads',
        'CDS.ParSpreads','UpfrontFlat','CDS.Upfront','ParSpreadFlat','CDS.ParSpreadFlat',
        'DefaultAccrual','CDS.DefaultAccrual','FeeLegFlows','CDS.FeeLegFlows',
        'DatesAndRates','CdsModelVersion'
    )
    $probe = $app.Workbooks.Add()
    $missing = @()
    foreach ($n in $names) {
        $probe.Sheets.Item(1).Range('A1').Formula = "=$n()"
        if ([string]$probe.Sheets.Item(1).Range('A1').Text -eq '#NAME?') { $missing += $n }
    }
    $probe.Close($false)
    "Registered   : $($names.Count - $missing.Count) / $($names.Count)"
    if ($missing.Count) { "NOT registered: $($missing -join ', ')"; $exit = 1 }

    $wb = $app.Workbooks.Open($Workbook)
    $app.CalculateFullRebuild()
    $ws = $wb.Sheets.Item(1)
    "B6 Discount  : $([string]$ws.Range('B6').Text)"
    "B7 Credit    : $([string]$ws.Range('B7').Text)"
    "B9 Version   : $([string]$ws.Range('B9').Text)"
    ''
    "{0,-24} {1,-14} {2,-8}" -f 'case', 'verdict (J)', 'forms (K)'
    $fail = 0
    for ($r = 25; $r -le 28; $r++) {
        $j = [string]$ws.Range("J$r").Text
        "{0,-24} {1,-14} {2,-8}" -f [string]$ws.Range("A$r").Text, $j, [string]$ws.Range("K$r").Text
        if ($j -notlike 'PASS*') { $fail = 1 }
    }
    $wb.Close($false)
    if ($fail) { $exit = 1 }
}
finally {
    $app.Quit()
    [System.Runtime.InteropServices.Marshal]::ReleaseComObject($app) | Out-Null
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
}
exit $exit
