# compile_oracle.ps1 - run witcher3.exe -verifyscriptsandexit and print the "Script Compilation Errors" dialog text.
# Clean compile: no dialog, process exits by itself. NOTE exit code is 3 in BOTH cases (clean and errors), so it is not
# a signal. The error dialog (Win32 class #32770, owned by witcher3.exe) shows at ~8 s and the process exits ~1-2 s
# later on its own, so a 2 s UI-Automation poll misses it: this version polls with Win32 EnumWindows every 50 ms and
# reads the dialog's child controls with WM_GETTEXT. Text is printed between
# "---- compilation dialog text ----" / "---- end ----"; the dialog is closed (WM_CLOSE) and our process stopped.
# Used by core\core_check.py. Never touches a witcher3.exe it did not start.
param([string]$Exe = 'G:\Games\The Witcher 3 Wild Hunt GOTY\bin\x64_dx12\witcher3.exe', [int]$TimeoutSec = 300)
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class OW {
  public delegate bool CB(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(CB c, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, CB c, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, StringBuilder l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<string> Titles(uint pid) { var r = new List<string>(); EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p); if(p==pid){ var s=new StringBuilder(512); GetWindowText(h,s,512); r.Add(h.ToInt64()+"|"+s); } return true; }, IntPtr.Zero); return r; }
  public static string Text(IntPtr h) {
    var s = new StringBuilder(65536); IntPtr res; SendMessageTimeout(h, 0x000D, (IntPtr)65535, s, 2, 500, out res); return s.ToString(); }
  public static List<string> Children(IntPtr hw) { var r = new List<string>();
    EnumChildWindows(hw, (h,l)=>{ var c=new StringBuilder(256); GetClassName(h,c,256); string t=Text(h); if(t.Length>0) r.Add(t); return true; }, IntPtr.Zero); return r; }
  public static void Close(IntPtr h) { PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
}
"@
$start = Get-Date
$p = Start-Process -FilePath $Exe -ArgumentList '-verifyscriptsandexit' -WorkingDirectory (Split-Path $Exe) -PassThru
"started pid $($p.Id)"
$seen = @{}
$dialog = $false
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 50
    foreach ($e in [OW]::Titles([uint32]$p.Id)) {
        $h, $t = $e.Split('|', 2)
        if (-not $seen.ContainsKey($t)) { $seen[$t] = 1; if ($t) { "[{0,5:N0}s] window: '{1}'" -f ((Get-Date) - $start).TotalSeconds, $t } }
        if ($t -like '*Script Compilation*') {
            $hw = [IntPtr][int64]$h
            "---- compilation dialog text ----"
            $txt = [OW]::Children($hw)
            if (-not $txt) { Start-Sleep -Milliseconds 300; $txt = [OW]::Children($hw) }
            $txt
            "---- end ----"
            $dialog = $true
            [OW]::Close($hw)
            break
        }
    }
    if ($dialog) { break }
}
if (-not $p.HasExited) {
    if (-not $dialog) { "timeout -> closing pid $($p.Id)" }
    $p.WaitForExit(3000) | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
$p.WaitForExit(15000) | Out-Null
"exit code: $($p.ExitCode)  elapsed: $([int]((Get-Date) - $start).TotalSeconds)s"
