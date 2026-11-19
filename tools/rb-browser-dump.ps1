# Development tool, read-only: dumps the rows of Rekordbox 7.2.19's browser list (what its knob moves through), as the
# Wave bridge finds them, with every text each row object points to. Used to learn where Rekordbox keeps a streamed
# (Beatport, Spotify) result's title, artist and ids for Wave's native browser. Reads Rekordbox's memory; changes nothing.
#   powershell -ExecutionPolicy Bypass -File rb-browser-dump.ps1 [-Rows 6] [-Tree]
#   -Tree: also walks Rekordbox's sidebar tree (objects, their class names, every text they reach), to find its items.
#   -Find "text": the chains of objects (with class names and offsets) from Rekordbox's browser view, and from its sidebar
#   tree, that lead to a text containing it, e.g. the title of a track Rekordbox shows now.
#   -Classes "regex": every object reachable from the browser view whose C++ class matches, with its chain, and for
#   list-like objects the rows / tracks fields the bridge reads for Rekordbox's own track list.
param([int]$Rows = 6, [switch]$Tree, [string]$Find = "", [string]$Classes = "", [int]$Depth = 6, [int]$Nodes = 150000)

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
    // Breadth-first through pointers from root: every chain that reaches a text containing needle.
    public static System.Collections.Generic.List<string> FindText(long root, string needle, int maxDepth, int maxNodes, long imageBase) {
        var found = new System.Collections.Generic.List<string>();
        var parent = new System.Collections.Generic.Dictionary<long, Tuple<long, int>>();
        var depth = new System.Collections.Generic.Dictionary<long, int>();
        var queue = new System.Collections.Generic.Queue<long>();
        queue.Enqueue(root); depth[root] = 0; parent[root] = null;
        int visited = 0;
        while (queue.Count > 0 && visited < maxNodes && found.Count < 40) {
            long obj = queue.Dequeue(); visited++;
            var raw = Read(obj, 0x400) ?? Read(obj, 0x100); if (raw == null) continue;
            for (int o = 0; o + 8 <= raw.Length; o += 8) {
                long v = BitConverter.ToInt64(raw, o);
                if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;
                string t = Text(v);
                if (t != null) {
                    if (t.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) {
                        var chain = new System.Collections.Generic.List<string>();
                        chain.Add(String.Format("+0x{0:x}='{1}'", o, t.Length > 80 ? t.Substring(0, 80) : t));
                        long cur = obj;
                        while (cur != 0 && parent.ContainsKey(cur)) {
                            var c = Class(cur, imageBase);
                            var p = parent[cur];
                            chain.Insert(0, String.Format("[0x{0:x} {1}]", cur, c ?? "?") + (p == null ? "" : ""));
                            if (p == null) break;
                            chain.Insert(0, String.Format("+0x{0:x}->", p.Item2));
                            cur = p.Item1;
                        }
                        found.Add(String.Join(" ", chain));
                    }
                    continue;
                }
                if ((v & 7) != 0 || depth[obj] >= maxDepth || depth.ContainsKey(v)) continue;
                depth[v] = depth[obj] + 1; parent[v] = Tuple.Create(obj, o); queue.Enqueue(v);
            }
        }
        found.Add(String.Format("(visited {0} objects)", visited));
        return found;
    }
    // Breadth-first: objects whose RTTI class matches the pattern, with their chain from root.
    public static System.Collections.Generic.List<string> FindClass(long root, string pattern, int maxDepth, int maxNodes, long imageBase) {
        var re = new System.Text.RegularExpressions.Regex(pattern);
        var found = new System.Collections.Generic.List<string>();
        var parent = new System.Collections.Generic.Dictionary<long, Tuple<long, int>>();
        var depth = new System.Collections.Generic.Dictionary<long, int>();
        var queue = new System.Collections.Generic.Queue<long>();
        queue.Enqueue(root); depth[root] = 0; parent[root] = null;
        int visited = 0;
        while (queue.Count > 0 && visited < maxNodes && found.Count < 400) {
            long obj = queue.Dequeue(); visited++;
            var raw = Read(obj, 0x400) ?? Read(obj, 0x100); if (raw == null) continue;
            string c = Class(obj, imageBase);
            if (c != null && re.IsMatch(c)) {
                var chain = new System.Text.StringBuilder();
                long cur = obj;
                var hops = new System.Collections.Generic.List<string>();
                while (parent.ContainsKey(cur) && parent[cur] != null) {
                    var p = parent[cur];
                    hops.Insert(0, String.Format("+0x{0:x}->{1}", p.Item2, Class(cur, imageBase) ?? "?"));
                    cur = p.Item1;
                }
                // The bridge's list fields: rows (+0x240), cursor box (+0x220), tracks array (+0x250: data, count at +0xc).
                long tracks = Q(obj + 0x250); long data = tracks != 0 ? Q(tracks) : 0; int stored = tracks != 0 ? I(tracks + 0xc) : 0;
                string first = data != 0 ? Class(Q(data), imageBase) : null;
                found.Add(String.Format("0x{0:x} {1} rows@240={2} stored={3} first={4} :: {5}", obj, c, I(obj + 0x240), stored, first,
                                        String.Join(" ", hops)));
            }
            for (int o = 0; o + 8 <= raw.Length; o += 8) {
                long v = BitConverter.ToInt64(raw, o);
                if (v < 0x10000 || v > 0x7FFFFFFFFFFF || (v & 7) != 0 || depth[obj] >= maxDepth || depth.ContainsKey(v)) continue;
                depth[v] = depth[obj] + 1; parent[v] = Tuple.Create(obj, o); queue.Enqueue(v);
            }
        }
        found.Add(String.Format("(visited {0} objects)", visited));
        return found;
    }
    // The C++ class of an object, from its vtable's RTTI (MSVC x64: vtable[-1] -> complete object locator).
    public static string Class(long obj, long imageBase) {
        long vt = Q(obj); if (vt < imageBase || vt > imageBase + 0x8000000) return null;
        long col = Q(vt - 8); if (col == 0 || I(col) != 1) return null;
        string name = Text(imageBase + (uint)I(col + 12) + 16);
        return name;
    }
    // A NUL-terminated UTF-8 text of at least 2 printable characters at addr, or null.
    public static string Text(long addr) {
        var b = Read(addr, 160); if (b == null) return null;
        int n = Array.IndexOf(b, (byte)0); if (n < 2) return null;
        var s = Encoding.UTF8.GetString(b, 0, n);
        foreach (char c in s) if (char.IsControl(c) || c == (char)0xFFFD) return null;
        return s;
    }
}
"@

$p = Get-Process rekordbox -ErrorAction Stop | Select-Object -First 1  # (its MainWindowHandle may be a dialog; not used)
$base = $p.MainModule.BaseAddress.ToInt64()
[Rb]::Open($p.Id)
"rekordbox pid $($p.Id) base 0x{0:x} version {1}" -f $base, $p.MainModule.FileVersionInfo.ProductVersion

$rel = { param($a) $a - 0x140000000 + $base }
$view = [Rb]::Q((& $rel 0x145f38738))
$lists = [Rb]::Q($view + 0x238); $container = [Rb]::Q($lists + 0x108); $list = [Rb]::Q($container + 0x110)
"view 0x{0:x} lists 0x{1:x} list 0x{2:x}" -f $view, $lists, $list
if ($list -eq 0) { "no browser list" }
$rowCount = [Rb]::I($list + 0x240); $order = [Rb]::Q($list + 0x228); $ordered = [Rb]::I($list + 0x234)
$tracks = [Rb]::Q($list + 0x250); $stored = [Rb]::I($tracks + 0xc); $data = [Rb]::Q($tracks)
"rows $rowCount ordered $ordered stored $stored"
if ($list -ne 0) { for ($row = 0; $row -lt [Math]::Min($Rows, $rowCount); $row++) {
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
} }

if ($Tree) {
    # Rekordbox's sidebar: the tree manager (+0x240 of the view) and its tree viewer (+0x100), walked breadth-first through
    # pointers (up to 4 levels, 4000 objects), printing each object's class and the texts its fields point at.
    $trees = [Rb]::Q($view + 0x240); $viewer = [Rb]::Q($trees + 0x100)
    "=== tree manager 0x{0:x} ({1}) viewer 0x{2:x} ({3})" -f $trees, [Rb]::Class($trees, $base), $viewer, [Rb]::Class($viewer, $base)
    $seen = @{}; $queue = New-Object System.Collections.Queue; $queue.Enqueue(@($viewer, 0, "viewer")); $count = 0
    while ($queue.Count -gt 0 -and $count -lt 4000) {
        $node = $queue.Dequeue(); $obj = $node[0]; $depth = $node[1]; $path = $node[2]
        if ($seen.ContainsKey($obj)) { continue }; $seen[$obj] = 1; $count++
        $raw = [Rb]::Read($obj, 0x300); if (-not $raw) { continue }
        $cls = [Rb]::Class($obj, $base)
        $texts = @()
        for ($o = 0; $o -lt 0x300; $o += 8) {
            $v = [BitConverter]::ToInt64($raw, $o)
            $s = [Rb]::Text($v)
            if ($s) { $texts += ("+0x{0:x}='{1}'" -f $o, $s) }
            elseif ($depth -lt 4 -and $v -gt 0x10000 -and $v -lt 0x7FFFFFFFFFFF -and ($v % 8) -eq 0) { $queue.Enqueue(@($v, ($depth + 1), ("{0}+0x{1:x}" -f $path, $o))) }
        }
        if ($cls -or $texts.Count) { "{0} 0x{1:x} {2} {3}" -f $path, $obj, $cls, ($texts -join ' ') }
    }
}

if ($Find) {
    "=== '$Find' from the browser view 0x{0:x} ({1})" -f $view, [Rb]::Class($view, $base)
    [Rb]::FindText($view, $Find, $Depth, $Nodes, $base) | ForEach-Object { $_ }
    $viewer = [Rb]::Q([Rb]::Q($view + 0x240) + 0x100)
    "=== '$Find' from the sidebar tree viewer 0x{0:x}" -f $viewer
    [Rb]::FindText($viewer, $Find, $Depth, $Nodes, $base) | ForEach-Object { $_ }
}

if ($Classes) {
    "=== classes /$Classes/ from the browser view 0x{0:x}" -f $view
    [Rb]::FindClass($view, $Classes, $Depth, $Nodes, $base) | ForEach-Object { $_ }
}
