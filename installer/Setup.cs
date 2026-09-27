using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyVersion("0.1.0.0")]
[assembly: AssemblyFileVersion("0.1.0.0")]

// Product.g.cs is generated from the exact payload hashes by build-installer.ps1.
internal static class Setup
{
    private const string Folder = "L4D2-DXVK-RT";
    private const string ProductId = "L4D2-DXVK-RT-Research";
    private const string Uninstaller = "uninstall.exe";
    private const string Marker = "install-state.txt";
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
    private static void WriteLog(string path, string line) { if (path != null) File.AppendAllText(path, DateTime.UtcNow.ToString("o") + " " + line + Environment.NewLine); }
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
    private static void Install(string root)
    {
        using (RegistryKey hive = UserHive())
        using (RegistryKey existing = hive.OpenSubKey(RegPath))
            if (existing != null) throw new IOException("Already installed. Uninstall the old version first.");
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
        }
        catch
        {
            if (registered) using (RegistryKey hive = UserHive()) hive.DeleteSubKeyTree(RegPath, false);
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
        using (FileStream locked = new FileStream(uninstall, FileMode.Open, FileAccess.Read, FileShare.None)) { }
        foreach (KeyValuePair<string, string> item in Product.Manifest) File.Delete(Child(root, item.Key));
        File.Delete(uninstall);
        File.Delete(marker);
        using (RegistryKey hive = UserHive()) hive.DeleteSubKeyTree(RegPath, false);
        RemoveEmptyTree(root); // Extra user files are retained.
    }
}
