using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyVersion("0.1.3.0")]
[assembly: AssemblyFileVersion("0.1.3.0")]

// Product.g.cs is generated from the exact payload hashes by build-installer.ps1.
internal static class Setup
{
    private const string Folder = "L4D2-DXVK-RT";
    private const string ProductId = "L4D2-DXVK-RT-Research";
    private const string Uninstaller = "uninstall.exe";
    private const string Marker = "install-state.txt";
    private const string MainProgram = "L4D2-DXVK-RT.exe";
    private const string StartMenuFolder = "L4D2 DXVK RT";
    private const string StartMenuShortcut = "L4D2 DXVK RT.lnk";
    private const string DesktopShortcut = "L4D2 DXVK RT.lnk";
    private const string RegPath = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + ProductId;
    private static string Self { get { return Assembly.GetExecutingAssembly().Location; } }
    private static string DefaultRoot { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", Folder); } }

    [STAThread]
    private static int Main(string[] args)
    {
        bool uninstall = string.Equals(Path.GetFileName(Self), Uninstaller, StringComparison.OrdinalIgnoreCase);
        bool silent = false;
        string root = null, log = null;
        int parent = 0;
        try
        {
            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--uninstall": uninstall = true; break;
                    case "--silent": silent = true; break;
                    case "--dir": if (++i == args.Length) throw new ArgumentException("--dir needs a path"); root = args[i]; break;
                    case "--log": if (++i == args.Length) throw new ArgumentException("--log needs a path"); log = args[i]; break;
                    case "--parent": if (++i == args.Length || !int.TryParse(args[i], out parent)) throw new ArgumentException("Invalid --parent"); break;
                    default: throw new ArgumentException("Unknown option: " + args[i]);
                }
            }
            if (!Environment.Is64BitOperatingSystem) throw new IOException("Requires 64-bit Windows.");
            if (uninstall && root == null) root = RegisteredRoot();
            if (!uninstall && root == null) root = DefaultRoot;
            root = SafeRoot(root);
            if (uninstall && Same(Self, Path.Combine(root, Uninstaller)))
            {
                // Windows cannot delete a running executable. A byte-identical worker
                // waits for this process to exit, then removes only manifest-owned files.
                string worker = Path.Combine(Path.GetTempPath(), Folder + "-uninstall-" + Guid.NewGuid().ToString("N") + ".exe");
                File.Copy(Self, worker, false);
                string command = "--uninstall --dir " + Quote(root) + " --parent " + Process.GetCurrentProcess().Id;
                if (silent) command += " --silent";
                if (log != null) command += " --log " + Quote(log);
                Process.Start(new ProcessStartInfo(worker, command) { UseShellExecute = false, CreateNoWindow = silent });
                return 0;
            }
            if (parent != 0)
            {
                try { using (Process p = Process.GetProcessById(parent)) if (!p.WaitForExit(30000)) throw new IOException("Uninstall launcher did not exit."); }
                catch (ArgumentException) { }
            }
            Application.EnableVisualStyles();
            if (!silent)
            {
                string verb = uninstall ? "卸载" : "安装";
                string warning = uninstall
                    ? "只删除安装器拥有且哈希未变化的文件；其他文件会保留。"
                    : "安装到独立用户目录，不会永久修改 L4D2。游戏接入仅供 -insecure 离线测试；不保证 VAC 安全或可玩光追。";
                if (!uninstall)
                {
                    using (FolderBrowserDialog dialog = new FolderBrowserDialog())
                    {
                        dialog.Description = "选择父目录（安装器会创建 " + Folder + "）";
                        dialog.SelectedPath = Path.GetDirectoryName(root);
                        if (dialog.ShowDialog() != DialogResult.OK) return 0;
                        root = SafeRoot(Path.Combine(dialog.SelectedPath, Folder));
                    }
                }
                if (MessageBox.Show(warning + "\n\n位置：" + root + "\n\n继续" + verb + "？",
                    "L4D2 DXVK RT — " + verb, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return 0;
            }
            using (Mutex mutex = new Mutex(false, @"Local\" + ProductId + "-Install"))
            {
                bool acquired;
                try { acquired = mutex.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
                if (!acquired) throw new IOException("Another install or uninstall is running.");
                try { if (uninstall) Remove(root); else Install(root); }
                finally { mutex.ReleaseMutex(); }
            }
            WriteLog(log, "RESULT=SUCCESS");
            if (!silent) MessageBox.Show(uninstall ? "卸载完成。" : "安装完成。可在 Windows 设置 → 已安装的应用中卸载，或运行安装目录中的 uninstall.exe。",
                "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        }
        catch (Exception e)
        {
            WriteLog(log, "RESULT=FAILED " + e.Message);
            if (!silent) MessageBox.Show(e.Message, "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static string Quote(string s) { return "\"" + s.Replace("\"", "") + "\""; }
    private static bool Same(string a, string b) { return string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase); }
    private static void WriteLog(string path, string line)
    {
        if (path == null) return;
        try
        {
            string parent = Path.GetDirectoryName(Path.GetFullPath(path));
            if (!String.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            File.AppendAllText(path, DateTime.UtcNow.ToString("o") + " " + line + Environment.NewLine);
        }
        catch (Exception) { /* diagnostics must never turn a real result into a second failure */ }
    }
    private static RegistryKey UserHive() { return RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64); }
    private static string RegisteredRoot()
    {
        using (RegistryKey hive = UserHive())
        using (RegistryKey key = hive.OpenSubKey(RegPath))
        {
            if (key == null) throw new IOException("No installation record found.");
            string root = key.GetValue("InstallLocation") as string;
            if (string.IsNullOrEmpty(root)) throw new IOException("Installation record has no location.");
            return root;
        }
    }
    private static bool HasStaleRegistration()
    {
        using (RegistryKey hive = UserHive())
        using (RegistryKey key = hive.OpenSubKey(RegPath))
        {
            if (key == null) return false;
            string root = key.GetValue("InstallLocation") as string;
            if (String.IsNullOrEmpty(root)) return true;
            string marker = Path.Combine(root, Marker);
            string uninstall = Path.Combine(root, Uninstaller);
            // Only remove a record when the entire package location is gone.
            // A partially damaged installation must remain user-visible so it
            // can be repaired or manually inspected instead of being deleted.
            return !Directory.Exists(root) && !File.Exists(marker) && !File.Exists(uninstall);
        }
    }
    private static void RemoveStaleRegistration()
    {
        using (RegistryKey hive = UserHive())
            hive.DeleteSubKeyTree(RegPath, false);
    }
    private static string Hash(string path)
    {
        using (SHA256 sha = SHA256.Create())
        using (FileStream stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
    }
    private static void NoReparse(string path)
    {
        for (string p = path; !string.IsNullOrEmpty(p); p = Path.GetDirectoryName(p))
            if ((File.Exists(p) || Directory.Exists(p)) && (File.GetAttributes(p) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Reparse point refused: " + p);
    }
    private static string SafeRoot(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || !Path.IsPathRooted(value) || value.StartsWith(@"\\"))
            throw new IOException("Choose a local absolute directory.");
        string root = Path.GetFullPath(value).TrimEnd('\\');
        if (root.Length < 4 || root.Substring(2).Contains(":") ||
            !string.Equals(Path.GetFileName(root), Folder, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Installation directory must end with " + Folder);
        NoReparse(root);
        for (string p = root; !string.IsNullOrEmpty(p); p = Path.GetDirectoryName(p))
            if (File.Exists(Path.Combine(p, "left4dead2.exe")) ||
                string.Equals(Path.GetFileName(p), "steamapps", StringComparison.OrdinalIgnoreCase))
                throw new IOException("Do not install the persistent package into a Steam game directory.");
        return root;
    }
    private static string Child(string root, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative) || Path.IsPathRooted(relative) || relative.Contains(":") || relative.Contains(".."))
            throw new IOException("Invalid payload path.");
        string path = Path.GetFullPath(Path.Combine(root, relative));
        if (!path.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase)) throw new IOException("Payload path escaped root.");
        NoReparse(path);
        return path;
    }
    private static void VerifyPayload(string root)
    {
        foreach (KeyValuePair<string, string> item in Product.Manifest)
        {
            string path = Child(root, item.Key);
            if (!File.Exists(path) || Hash(path) != item.Value) throw new IOException("Modified or missing installed file: " + path);
            if ((File.GetAttributes(path) & FileAttributes.ReadOnly) != 0) throw new IOException("Read-only installed file: " + path);
            using (FileStream locked = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None)) { }
        }
    }
    private static void RemoveEmptyTree(string root)
    {
        if (!Directory.Exists(root)) return;
        NoReparse(root);
        foreach (string dir in Directory.GetDirectories(root)) RemoveEmptyTree(dir);
        if (Directory.GetFileSystemEntries(root).Length == 0) Directory.Delete(root, false);
    }
    private static string StartMenuShortcutPath()
    {
        string startMenu = Environment.GetFolderPath(Environment.SpecialFolder.StartMenu);
        return Path.Combine(startMenu, "Programs", StartMenuFolder, StartMenuShortcut);
    }
    private static string DesktopShortcutPath()
    {
        string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        return String.IsNullOrEmpty(desktop) ? null : Path.Combine(desktop, DesktopShortcut);
    }
    private static List<string> ShortcutPaths()
    {
        List<string> result = new List<string> { StartMenuShortcutPath() };
        string desktop = DesktopShortcutPath();
        if (!String.IsNullOrEmpty(desktop)) result.Add(desktop);
        return result;
    }
    private static void SetComProperty(object target, string property, object value)
    {
        target.GetType().InvokeMember(property, BindingFlags.Instance | BindingFlags.Public | BindingFlags.SetProperty,
            null, target, new object[] { value });
    }
    private static string GetShortcutTarget(string path)
    {
        if (!File.Exists(path)) return null;
        object shell = null, shortcut = null;
        try
        {
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType == null) return null;
            shell = Activator.CreateInstance(shellType);
            shortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.Instance | BindingFlags.Public | BindingFlags.InvokeMethod,
                null, shell, new object[] { path });
            object value = shortcut.GetType().InvokeMember("TargetPath", BindingFlags.Instance | BindingFlags.Public | BindingFlags.GetProperty,
                null, shortcut, null);
            return value as string;
        }
        catch (Exception) { return null; }
        finally
        {
            if (shortcut != null && Marshal.IsComObject(shortcut)) Marshal.FinalReleaseComObject(shortcut);
            if (shell != null && Marshal.IsComObject(shell)) Marshal.FinalReleaseComObject(shell);
        }
    }
    private static void CreateShortcut(string path, string target)
    {
        string existing = GetShortcutTarget(path);
        if (File.Exists(path) && (existing == null || !Same(existing, target)))
            throw new IOException("Shortcut already exists and points to another program: " + path);
        string parent = Path.GetDirectoryName(path);
        if (!String.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
        object shell = null, shortcut = null;
        try
        {
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType == null) throw new IOException("Windows shortcut service is unavailable.");
            shell = Activator.CreateInstance(shellType);
            shortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.Instance | BindingFlags.Public | BindingFlags.InvokeMethod,
                null, shell, new object[] { path });
            SetComProperty(shortcut, "TargetPath", target);
            SetComProperty(shortcut, "WorkingDirectory", Path.GetDirectoryName(target));
            SetComProperty(shortcut, "Arguments", "--start");
            SetComProperty(shortcut, "Description", "L4D2 DXVK RT offline launcher");
            SetComProperty(shortcut, "IconLocation", target + ",0");
            shortcut.GetType().InvokeMember("Save", BindingFlags.Instance | BindingFlags.Public | BindingFlags.InvokeMethod,
                null, shortcut, null);
        }
        finally
        {
            if (shortcut != null && Marshal.IsComObject(shortcut)) Marshal.FinalReleaseComObject(shortcut);
            if (shell != null && Marshal.IsComObject(shell)) Marshal.FinalReleaseComObject(shell);
        }
    }
    private static void CreateShortcuts(string root)
    {
        string target = Child(root, MainProgram);
        if (!File.Exists(target)) throw new IOException("Installed main program is missing: " + target);
        foreach (string path in ShortcutPaths()) CreateShortcut(path, target);
    }
    private static void RemoveShortcuts(string root)
    {
        string target = Path.Combine(root, MainProgram);
        foreach (string path in ShortcutPaths())
        {
            if (String.Equals(GetShortcutTarget(path), target, StringComparison.OrdinalIgnoreCase))
            {
                try { File.Delete(path); } catch (FileNotFoundException) { }
            }
        }
        string startFolder = Path.GetDirectoryName(StartMenuShortcutPath());
        if (!String.IsNullOrEmpty(startFolder)) RemoveEmptyTree(startFolder);
    }
    private static void Install(string root)
    {
        if (HasStaleRegistration())
        {
            RemoveStaleRegistration();
        }
        using (RegistryKey hive = UserHive())
        using (RegistryKey existing = hive.OpenSubKey(RegPath))
            if (existing != null) throw new IOException("Already installed. Uninstall the old version first, or choose a different package directory.");
        if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Target directory already exists; nothing will be overwritten.");
        string parent = Path.GetDirectoryName(root);
        Directory.CreateDirectory(parent);
        string stage = Path.Combine(parent, "." + Folder + "-stage-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(stage);
        bool moved = false, registered = false;
        try
        {
            HashSet<string> extracted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (Stream resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            using (ZipArchive zip = new ZipArchive(resource, ZipArchiveMode.Read))
            {
                foreach (ZipArchiveEntry entry in zip.Entries)
                {
                    string relative = entry.FullName.Replace('/', '\\');
                    string expected;
                    if (!Product.Manifest.TryGetValue(relative, out expected) || !extracted.Add(relative))
                        throw new IOException("Payload manifest mismatch.");
                    string path = Child(stage, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    using (Stream input = entry.Open())
                    using (FileStream output = new FileStream(path, FileMode.CreateNew)) input.CopyTo(output);
                    if (Hash(path) != expected) throw new IOException("Payload hash mismatch: " + relative);
                }
            }
            if (extracted.Count != Product.Manifest.Count) throw new IOException("Incomplete payload.");
            File.Copy(Self, Child(stage, Uninstaller), false);
            string marker = "product=" + ProductId + "\r\nversion=" + Product.Version + "\r\nroot=" + root + "\r\n";
            File.WriteAllText(Child(stage, Marker), marker);
            if (Directory.Exists(root) || File.Exists(root)) throw new IOException("Target appeared during install.");
            Directory.Move(stage, root);
            moved = true;
            using (RegistryKey hive = UserHive())
            using (RegistryKey key = hive.CreateSubKey(RegPath))
            {
                registered = true;
                key.SetValue("DisplayName", "L4D2 DXVK RT (Research / Offline)");
                key.SetValue("DisplayVersion", Product.Version);
                key.SetValue("Publisher", "chen121207");
                key.SetValue("InstallLocation", root);
                key.SetValue("UninstallString", Quote(Child(root, Uninstaller)) + " --uninstall");
                key.SetValue("QuietUninstallString", Quote(Child(root, Uninstaller)) + " --uninstall --silent");
                key.SetValue("DisplayIcon", Child(root, Uninstaller));
                key.SetValue("URLInfoAbout", "https://github.com/chen121207/l4d2-dxvk-optimize");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
                key.SetValue("MarkerSHA256", Hash(Child(root, Marker)));
                key.SetValue("UninstallerSHA256", Hash(Child(root, Uninstaller)));
            }
            CreateShortcuts(root);
        }
        catch
        {
            if (registered) using (RegistryKey hive = UserHive()) hive.DeleteSubKeyTree(RegPath, false);
            RemoveShortcuts(root);
            string owned = moved ? root : stage;
            if (Directory.Exists(owned))
            {
                foreach (KeyValuePair<string, string> item in Product.Manifest)
                {
                    string path = Child(owned, item.Key);
                    if (File.Exists(path) && Hash(path) == item.Value) File.Delete(path);
                }
                string uninstall = Child(owned, Uninstaller);
                if (File.Exists(uninstall) && Hash(uninstall) == Hash(Self)) File.Delete(uninstall);
                string marker = Child(owned, Marker);
                if (File.Exists(marker)) File.Delete(marker);
                RemoveEmptyTree(owned);
            }
            throw;
        }
    }
    private static void Remove(string root)
    {
        if (!Same(root, RegisteredRoot())) throw new IOException("Installation directory and registry disagree.");
        string marker = Child(root, Marker), uninstall = Child(root, Uninstaller);
        using (RegistryKey hive = UserHive())
        using (RegistryKey key = hive.OpenSubKey(RegPath))
        {
            if (key == null || !File.Exists(marker) || Hash(marker) != (key.GetValue("MarkerSHA256") as string) ||
                !File.Exists(uninstall) || Hash(uninstall) != (key.GetValue("UninstallerSHA256") as string) ||
                Hash(Self) != Hash(uninstall)) throw new IOException("Installation identity verification failed.");
        }
        string expectedMarker = "product=" + ProductId + "\r\nversion=" + Product.Version + "\r\nroot=" + root + "\r\n";
        if (File.ReadAllText(marker) != expectedMarker) throw new IOException("Installation marker mismatch.");
        foreach (Process p in Process.GetProcessesByName("left4dead2"))
            using (p) throw new IOException("Close L4D2 before uninstalling the runtime.");
        VerifyPayload(root);
        RemoveShortcuts(root);
        using (FileStream locked = new FileStream(uninstall, FileMode.Open, FileAccess.Read, FileShare.None)) { }
        foreach (KeyValuePair<string, string> item in Product.Manifest) File.Delete(Child(root, item.Key));
        File.Delete(uninstall);
        File.Delete(marker);
        using (RegistryKey hive = UserHive()) hive.DeleteSubKeyTree(RegPath, false);
        RemoveEmptyTree(root); // Extra user files are retained.
    }
}
