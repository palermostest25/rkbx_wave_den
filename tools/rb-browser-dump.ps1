# Development tool, read-only: dumps the rows of Rekordbox 7.2.19's browser list (what its knob moves through), as the
# Wave bridge finds them, with every text each row object points to. Used to learn where Rekordbox keeps a streamed
# (Beatport, Spotify) result's title, artist and ids for Wave's native browser. Reads Rekordbox's memory; changes nothing.
#   powershell -ExecutionPolicy Bypass -File rb-browser-dump.ps1 [-Rows 6] [-Select "Beatport"]
param([int]$Rows = 6, [string]$Select = "")

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Rb {
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    static IntPtr proc;
    public static void Open(int pid) { proc = OpenProcess(0x0410, false, pid); if (proc == IntPtr.Zero) throw new Exception("OpenProcess failed"); }
    public static byte[] Read(long addr, int n) {
        var b = new byte[n]; IntPtr got;
        if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF || !ReadProcessMemory(proc, new IntPtr(addr), b, new IntPtr(n), out got) || got.ToInt64() != n) return null;
        return b;
    }
    public static long Q(long addr) { var b = Read(addr, 8); return b == null ? 0 : BitConverter.ToInt64(b, 0); }
    public static int I(long addr) { var b = Read(addr, 4); return b == null ? 0 : BitConverter.ToInt32(b, 0); }
    // A NUL-terminated UTF-8 text of at least 2 printable characters at addr, or null.
    public static string Text(long addr) {
        var b = Read(addr, 160); if (b == null) return null;
        int n = Array.IndexOf(b, (byte)0); if (n < 2) return null;
        var s = Encoding.UTF8.GetString(b, 0, n);
        foreach (char c in s) if (char.IsControl(c) || c == '�') return null;
        return s;
    }
}
"@

$p = Get-Process rekordbox -ErrorAction Stop | Select-Object -First 1
$base = $p.MainModule.BaseAddress.ToInt64()
[Rb]::Open($p.Id)
"rekordbox pid $($p.Id) base 0x{0:x} version {1}" -f $base, $p.MainModule.FileVersionInfo.ProductVersion

if ($Select) {
    # Rekordbox's tree item by name (as Wave does), through UI Automation, then a moment for its list to fill.
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $Select)
    $item = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    if ($item) {
        "selecting '$Select' ($($item.Current.ControlType.ProgrammaticName))"
        $sel = $null
        if ($item.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) { $sel.Select() }
        else { $inv = $null; if ($item.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$inv)) { $inv.Invoke() } }
        Start-Sleep -Seconds 4
    } else { "no UI Automation element named '$Select' in Rekordbox's main window" }
}

$rel = { param($a) $a - 0x140000000 + $base }
$view = [Rb]::Q((& $rel 0x145f38738))
$lists = [Rb]::Q($view + 0x238); $container = [Rb]::Q($lists + 0x108); $list = [Rb]::Q($container + 0x110)
"view 0x{0:x} lists 0x{1:x} list 0x{2:x}" -f $view, $lists, $list
if ($list -eq 0) { "no browser list"; exit }
$rowCount = [Rb]::I($list + 0x240); $order = [Rb]::Q($list + 0x228); $ordered = [Rb]::I($list + 0x234)
$tracks = [Rb]::Q($list + 0x250); $stored = [Rb]::I($tracks + 0xc); $data = [Rb]::Q($tracks)
"rows $rowCount ordered $ordered stored $stored"
for ($row = 0; $row -lt [Math]::Min($Rows, $rowCount); $row++) {
    $index = if ($row -lt $ordered -and $order) { [Rb]::I($order + 4 * $row) } else { 0 }
    $t = [Rb]::Q($data + 8 * $index)
    "=== row $row index $index object 0x{0:x} vtable 0x{1:x} id {2}" -f $t, ([Rb]::Q($t) - $base + 0x140000000), [Rb]::I($t + 8)
    $raw = [Rb]::Read($t, 0x4e0)
    if (-not $raw) { "  unreadable"; continue }
    for ($o = 0; $o -lt 0x4e0; $o += 8) {
        $v = [BitConverter]::ToInt64($raw, $o)
        $s = [Rb]::Text($v)
        if ($s) { "  +0x{0:x3} -> '{1}'" -f $o, $s; continue }
        # One level deeper: an object whose first fields point at text (a juce::String inside it, a nested record).
        $inner = [Rb]::Read($v, 0x40)
        if ($inner) { for ($k = 0; $k -lt 0x40; $k += 8) { $s2 = [Rb]::Text([BitConverter]::ToInt64($inner, $k)); if ($s2) { "  +0x{0:x3} -> +0x{1:x2} -> '{2}'" -f $o, $k, $s2 } } }
    }
    "  ints: " + ((0..31 | ForEach-Object { "{0:x2}={1}" -f ($_*4), [BitConverter]::ToInt32($raw, $_*4) }) -join ' ')
}
