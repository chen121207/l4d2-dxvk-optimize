using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;
using System.Xml.Serialization;

// The launcher is deliberately separate from Setup.cs.  Setup installs this
// executable and creates its shortcut; this program owns only the per-user
// game selection and starts the offline PowerShell session.
[Serializable]
[XmlRoot("L4D2DxvkRtConfiguration")]
public sealed class LauncherConfiguration
{
    [XmlElement("Version")]
    public int Version = 1;

    [XmlElement("GameDirectory")]
    public string GameDirectory = string.Empty;

    [XmlElement("EnableFFG")]
    public bool EnableFFG;

    [XmlElement("UpdatedUtc")]
    public string UpdatedUtc = string.Empty;
}

internal static class LauncherPaths
{
    internal static string InstallRoot
    {
        get { return AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar); }
    }

    internal static string Script { get { return Path.Combine(InstallRoot, "tools\\Launch-Offline.ps1"); } }
    internal static string Session { get { return Path.Combine(InstallRoot, "offline-session.json"); } }
    internal static string Uninstaller { get { return Path.Combine(InstallRoot, "uninstall.exe"); } }

    internal static string UserDirectory
    {
        get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "L4D2-DXVK-RT"); }
    }

    internal static string ConfigurationFile { get { return Path.Combine(UserDirectory, "config.xml"); } }
    internal static string LogFile { get { return Path.Combine(UserDirectory, "launcher.log"); } }
}

internal static class LauncherConfigurationStore
{
    internal static LauncherConfiguration Load()
    {
        string file = LauncherPaths.ConfigurationFile;
        if (!File.Exists(file)) return new LauncherConfiguration();
        try
        {
            XmlSerializer serializer = new XmlSerializer(typeof(LauncherConfiguration));
            using (FileStream stream = File.OpenRead(file))
            {
                LauncherConfiguration result = serializer.Deserialize(stream) as LauncherConfiguration;
                if (result == null || result.Version != 1) return new LauncherConfiguration();
                return result;
            }
        }
        catch
        {
            // A broken per-user file must not make the launcher unusable.  The
            // next run opens the picker and replaces it after confirmation.
            return new LauncherConfiguration();
        }
    }

    internal static void Save(LauncherConfiguration configuration)
    {
        Directory.CreateDirectory(LauncherPaths.UserDirectory);
        configuration.Version = 1;
        configuration.UpdatedUtc = DateTime.UtcNow.ToString("o");
        string temporary = LauncherPaths.ConfigurationFile + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            XmlSerializer serializer = new XmlSerializer(typeof(LauncherConfiguration));
            using (FileStream stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                serializer.Serialize(stream, configuration);
            if (File.Exists(LauncherPaths.ConfigurationFile))
                File.Replace(temporary, LauncherPaths.ConfigurationFile, null);
            else
                File.Move(temporary, LauncherPaths.ConfigurationFile);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    internal static void Log(string line)
    {
        try
        {
            Directory.CreateDirectory(LauncherPaths.UserDirectory);
            File.AppendAllText(LauncherPaths.LogFile,
                DateTime.UtcNow.ToString("o") + " " + line + Environment.NewLine, Encoding.UTF8);
        }
        catch { }
    }
}

internal static class GameDirectoryValidation
{
    internal static bool IsL4D2(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return false;
        try
        {
            string directory = Normalize(value);
            return Directory.Exists(directory) &&
                File.Exists(Path.Combine(directory, "left4dead2.exe")) &&
                File.Exists(Path.Combine(directory, "bin\\shaderapidx9.dll"));
        }
        catch { return false; }
    }

    internal static string Normalize(string value)
    {
        string full = Path.GetFullPath(value.Trim());
        return full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }
}

internal sealed class LauncherForm : Form
{
    private readonly LauncherConfiguration configuration;
    private readonly bool configureRequested;
    private readonly bool startRequested;
    private readonly TextBox gameDirectoryBox;
    private readonly CheckBox ffgBox;
    private readonly Label statusLabel;
    private Process launchProcess;
    private bool selecting;

    internal LauncherForm(string[] arguments)
    {
        configureRequested = HasArgument(arguments, "--configure");
        startRequested = HasArgument(arguments, "--start");
        configuration = LauncherConfigurationStore.Load();

        Text = "L4D2 DXVK RT";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(560, 250);
        ClientSize = new Size(620, 290);
        Font = new Font("Microsoft YaHei UI", 9F);

        Label title = new Label {
            AutoSize = true,
            Left = 18,
            Top = 18,
            Text = "L4D2 DXVK RT（离线测试启动器）",
            Font = new Font(Font, FontStyle.Bold)
        };
        Controls.Add(title);

        Label pathLabel = new Label { AutoSize = true, Left = 18, Top = 62, Text = "L4D2 游戏目录：" };
        Controls.Add(pathLabel);

        gameDirectoryBox = new TextBox {
            Left = 18,
            Top = 84,
            Width = 470,
            ReadOnly = true,
            Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right
        };
        Controls.Add(gameDirectoryBox);

        Button chooseButton = new Button { Left = 500, Top = 82, Width = 100, Text = "重新选择" };
        chooseButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        chooseButton.Click += delegate { ChooseGameDirectory(); };
        Controls.Add(chooseButton);

        ffgBox = new CheckBox {
            Left = 18,
            Top = 125,
            AutoSize = true,
            Text = "启用 FreeFrameGen（实验功能）",
            Checked = configuration.EnableFFG
        };
        ffgBox.CheckedChanged += delegate { SaveConfiguration(false); };
        Controls.Add(ffgBox);

        Button startButton = new Button { Left = 18, Top = 165, Width = 145, Height = 32, Text = "启动 L4D2（离线）" };
        startButton.Click += delegate { StartOffline(false); };
        Controls.Add(startButton);

        Button recoverButton = new Button { Left = 175, Top = 165, Width = 125, Height = 32, Text = "恢复临时会话" };
        recoverButton.Click += delegate { StartOffline(true); };
        Controls.Add(recoverButton);

        Button configButton = new Button { Left = 310, Top = 165, Width = 125, Height = 32, Text = "打开配置目录" };
        configButton.Click += delegate { OpenConfigurationDirectory(); };
        Controls.Add(configButton);

        Button uninstallButton = new Button { Left = 445, Top = 165, Width = 155, Height = 32, Text = "卸载 L4D2 DXVK RT" };
        uninstallButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        uninstallButton.Click += delegate { StartUninstaller(); };
        Controls.Add(uninstallButton);

        statusLabel = new Label {
            Left = 18,
            Top = 220,
            Width = 580,
            Height = 48,
            Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right,
            ForeColor = Color.DimGray,
            AutoEllipsis = true,
            Text = "正在读取配置……"
        };
        Controls.Add(statusLabel);

        Shown += OnShown;
        FormClosing += delegate { SaveConfiguration(false); };
    }

    private static bool HasArgument(string[] arguments, string wanted)
    {
        foreach (string argument in arguments)
            if (string.Equals(argument, wanted, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    private void OnShown(object sender, EventArgs e)
    {
        UpdatePathDisplay();
        bool valid = GameDirectoryValidation.IsL4D2(configuration.GameDirectory);
        if (configureRequested || !valid)
        {
            if (!ChooseGameDirectory())
            {
                statusLabel.Text = "尚未选择有效的 L4D2 目录。可以稍后点击“重新选择”。";
                return;
            }
        }
        else
        {
            statusLabel.Text = "已读取配置：" + configuration.GameDirectory;
        }
        if (startRequested) BeginInvoke((MethodInvoker)delegate { StartOffline(false); });
    }

    private bool ChooseGameDirectory()
    {
        if (selecting) return false;
        selecting = true;
        try
        {
            using (FolderBrowserDialog dialog = new FolderBrowserDialog())
            {
                dialog.Description = "选择 Left 4 Dead 2 的游戏目录（必须包含 left4dead2.exe 和 bin\\shaderapidx9.dll）";
                dialog.ShowNewFolderButton = false;
                if (GameDirectoryValidation.IsL4D2(configuration.GameDirectory))
                    dialog.SelectedPath = configuration.GameDirectory;
                while (dialog.ShowDialog(this) == DialogResult.OK)
                {
                    if (!GameDirectoryValidation.IsL4D2(dialog.SelectedPath))
                    {
                        MessageBox.Show(this,
                            "这个目录不是有效的 Left 4 Dead 2 安装目录。请选包含 left4dead2.exe 的目录。",
                            "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        continue;
                    }
                    configuration.GameDirectory = GameDirectoryValidation.Normalize(dialog.SelectedPath);
                    SaveConfiguration(true);
                    UpdatePathDisplay();
                    statusLabel.Text = "配置已保存。以后启动会直接使用此目录。";
                    return true;
                }
            }
            return false;
        }
        finally { selecting = false; }
    }

    private void UpdatePathDisplay()
    {
        gameDirectoryBox.Text = configuration.GameDirectory ?? string.Empty;
    }

    private void SaveConfiguration(bool showErrors)
    {
        if (!GameDirectoryValidation.IsL4D2(configuration.GameDirectory)) return;
        try { LauncherConfigurationStore.Save(configuration); }
        catch (Exception error)
        {
            LauncherConfigurationStore.Log("CONFIG_SAVE_FAILED " + error.Message);
            if (showErrors) MessageBox.Show(this, "配置保存失败：" + error.Message, "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void StartOffline(bool recover)
    {
        if (launchProcess != null)
        {
            try
            {
                if (!launchProcess.HasExited)
                {
                    statusLabel.Text = "启动脚本已经在运行。";
                    return;
                }
            }
            catch { }
        }
        if (!GameDirectoryValidation.IsL4D2(configuration.GameDirectory))
        {
            if (!ChooseGameDirectory()) return;
        }
        if (!File.Exists(LauncherPaths.Script))
        {
            MessageBox.Show(this, "安装文件不完整，缺少 tools\\Launch-Offline.ps1。请重新安装。",
                "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        if (Process.GetProcessesByName("left4dead2").Length != 0)
        {
            MessageBox.Show(this, "L4D2 已经在运行，请先关闭现有游戏。",
                "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        SaveConfiguration(true);
        string arguments = "-NoProfile -ExecutionPolicy Bypass -File " + Quote(LauncherPaths.Script) +
            " -GameDirectory " + Quote(configuration.GameDirectory);
        if (configuration.EnableFFG) arguments += " -EnableFFG";
        if (recover) arguments += " -Recover";
        try
        {
            ProcessStartInfo info = new ProcessStartInfo {
                FileName = "powershell.exe",
                Arguments = arguments,
                WorkingDirectory = LauncherPaths.InstallRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            launchProcess = new Process { StartInfo = info, EnableRaisingEvents = true };
            launchProcess.OutputDataReceived += OnProcessOutput;
            launchProcess.ErrorDataReceived += OnProcessOutput;
            launchProcess.Exited += OnProcessExited;
            if (!launchProcess.Start()) throw new IOException("无法启动 PowerShell。");
            launchProcess.BeginOutputReadLine();
            launchProcess.BeginErrorReadLine();
            statusLabel.Text = recover ? "正在恢复临时会话……" : "正在启动 L4D2（-insecure 离线模式）……";
            LauncherConfigurationStore.Log("START recover=" + recover + " game=" + configuration.GameDirectory);
        }
        catch (Exception error)
        {
            LauncherConfigurationStore.Log("START_FAILED " + error);
            MessageBox.Show(this, "启动失败：" + error.Message, "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OnProcessOutput(object sender, DataReceivedEventArgs e)
    {
        if (string.IsNullOrEmpty(e.Data)) return;
        LauncherConfigurationStore.Log("POWERSHELL " + e.Data);
        try { BeginInvoke((MethodInvoker)delegate { statusLabel.Text = e.Data; }); } catch { }
    }

    private void OnProcessExited(object sender, EventArgs e)
    {
        int code = -1;
        try { code = launchProcess.ExitCode; } catch { }
        LauncherConfigurationStore.Log("POWERSHELL_EXIT " + code);
        try { BeginInvoke((MethodInvoker)delegate { statusLabel.Text = code == 0 ? "游戏已退出，临时文件已恢复。" : "启动器退出，代码：" + code; }); } catch { }
    }

    private void OpenConfigurationDirectory()
    {
        try
        {
            Directory.CreateDirectory(LauncherPaths.UserDirectory);
            Process.Start(new ProcessStartInfo("explorer.exe", Quote(LauncherPaths.UserDirectory)) { UseShellExecute = true });
        }
        catch (Exception error) { MessageBox.Show(this, error.Message, "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }

    private void StartUninstaller()
    {
        if (launchProcess != null)
        {
            try
            {
                if (!launchProcess.HasExited)
                {
                    MessageBox.Show(this, "请先关闭正在运行的 L4D2 离线会话，再卸载。",
                        "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }
            }
            catch { }
        }
        if (!File.Exists(LauncherPaths.Uninstaller))
        {
            MessageBox.Show(this, "找不到 uninstall.exe。请从 Windows 设置卸载。", "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (MessageBox.Show(this, "确定要卸载 L4D2 DXVK RT 吗？", "L4D2 DXVK RT",
            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        try
        {
            Process.Start(new ProcessStartInfo(LauncherPaths.Uninstaller, "--uninstall") { UseShellExecute = true });
            Close();
        }
        catch (Exception error) { MessageBox.Show(this, error.Message, "L4D2 DXVK RT", MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }

    private static string Quote(string value)
    {
        return "\"" + (value ?? string.Empty).Replace("\"", string.Empty) + "\"";
    }
}

internal static class L4D2DxvkRtLauncher
{
    [STAThread]
    private static void Main(string[] arguments)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new LauncherForm(arguments));
    }
}
