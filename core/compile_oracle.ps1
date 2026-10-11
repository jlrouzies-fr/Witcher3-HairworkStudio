# compile_oracle.ps1 - run witcher3.exe -verifyscriptsandexit and print the "Script Compilation Errors" dialog text
# (UI Automation). Clean compile: the process exits by itself with code 3. On errors the dialog opens; its text is
# printed between "---- compilation dialog text ----" / "---- end ----" and the process (started here) is closed.
# Used by core\core_check.py. Never touches a witcher3.exe it did not start.
param([string]$Exe = 'G:\Games\The Witcher 3 Wild Hunt GOTY\bin\x64_dx12\witcher3.exe', [int]$TimeoutSec = 300)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$start = Get-Date
$p = Start-Process -FilePath $Exe -ArgumentList '-verifyscriptsandexit' -WorkingDirectory (Split-Path $Exe) -PassThru
"started pid $($p.Id)"
$seen = @{}
$dialog = $false
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Seconds 2
    $p.Refresh()
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
    $wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
    foreach ($w in $wins) {
        $t = $w.Current.Name
        if (-not $seen.ContainsKey($t)) { $seen[$t] = 1; "[{0,5:N0}s] window: '{1}'" -f ((Get-Date) - $start).TotalSeconds, $t }
        if ($t -like '*Script Compilation*') {
            Start-Sleep -Seconds 2
            "---- compilation dialog text ----"
            $all = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($e in $all) {
                $n = $e.Current.Name; if ($n) { $n }
                try { $vp = $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern); if ($vp.Current.Value) { $vp.Current.Value } } catch {}
            }
            "---- end ----"
            $dialog = $true
            Stop-Process -Id $p.Id -Force
            break
        }
    }
    if ($dialog) { break }
}
if (-not $p.HasExited -and -not $dialog) { "timeout -> closing pid $($p.Id)"; Stop-Process -Id $p.Id -Force }
$p.WaitForExit(15000) | Out-Null
"exit code: $($p.ExitCode)  elapsed: $([int]((Get-Date) - $start).TotalSeconds)s"
