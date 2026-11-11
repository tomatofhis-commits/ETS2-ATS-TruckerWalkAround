using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace TruckerWalkAround
{
    public class MainForm : Form
    {
        // -------------------------------------------------------------
        // プラグインが安全動作を確認済みの対応メジャーバージョン
        // -------------------------------------------------------------
        private static readonly string[] SupportedMajorVersions = new string[] { "1.62" };

        private ComboBox comboLang;
        private Label lblTitle;
        private GroupBox grpStatus;
        private Label lblAtsStatus;
        private Label lblAtsPath;
        private Label lblEtsStatus;
        private Label lblEtsPath;
        private Label lblHint;

        private GroupBox grpGuide;
        private TextBox txtGuide;
        private GroupBox grpLog;
        private TextBox txtLog;
        private Button btnClearLog;

        private System.Windows.Forms.Timer logTimer;
        private System.Windows.Forms.Timer processMonitorTimer;
        private long lastLogPosition = 0;
        private string logFilePath = "";

        private string atsDir = "";
        private string etsDir = "";
        private string atsVersion = "";
        private string etsVersion = "";
        private bool atsCompatible = false;
        private bool etsCompatible = false;

        private List<string> installedDllPaths = new List<string>();

        private bool isJapanese = true;
        private bool wasGameRunning = false;

        public MainForm()
        {
            this.AutoScaleMode = AutoScaleMode.Dpi;
            InitializeComponent();
            InitLanguage();
            DetectGamesAndInstall();
            InitLogWatcher();
            InitProcessMonitor();
        }

        private void InitializeComponent()
        {
            this.Text = "TruckerWalkAround Manager v1.0.1";
            try
            {
                string iconPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "TWAico.ico");
                if (File.Exists(iconPath))
                {
                    this.Icon = new Icon(iconPath);
                }
            }
            catch { }

            this.Size = new Size(760, 780);
            this.StartPosition = FormStartPosition.CenterScreen;
            this.FormBorderStyle = FormBorderStyle.FixedSingle;
            this.MaximizeBox = false;
            this.BackColor = Color.FromArgb(245, 246, 250);

            // Title
            lblTitle = new Label();
            lblTitle.Text = "TruckerWalkAround";
            lblTitle.Font = new Font("Segoe UI", 15, FontStyle.Bold);
            lblTitle.ForeColor = Color.FromArgb(44, 62, 80);
            lblTitle.Location = new Point(18, 14);
            lblTitle.AutoSize = true;
            this.Controls.Add(lblTitle);

            // Language Selector
            comboLang = new ComboBox();
            comboLang.DropDownStyle = ComboBoxStyle.DropDownList;
            comboLang.Items.Add("日本語 (Japanese)");
            comboLang.Items.Add("English");
            comboLang.SelectedIndex = 0;
            comboLang.Location = new Point(560, 16);
            comboLang.Size = new Size(165, 28);
            comboLang.Font = new Font("Segoe UI", 9.5f);
            comboLang.SelectedIndexChanged += (s, e) => {
                isJapanese = (comboLang.SelectedIndex == 0);
                ApplyLanguage();
            };
            this.Controls.Add(comboLang);

            // Status Group
            grpStatus = new GroupBox();
            grpStatus.Location = new Point(18, 52);
            grpStatus.Size = new Size(708, 175);
            grpStatus.Font = new Font("Segoe UI", 10.0f, FontStyle.Bold);
            this.Controls.Add(grpStatus);

            lblAtsStatus = new Label();
            lblAtsStatus.Location = new Point(15, 25);
            lblAtsStatus.Size = new Size(675, 22);
            lblAtsStatus.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            grpStatus.Controls.Add(lblAtsStatus);

            lblAtsPath = new Label();
            lblAtsPath.Location = new Point(32, 47);
            lblAtsPath.Size = new Size(660, 20);
            lblAtsPath.Font = new Font("Segoe UI", 8.5f, FontStyle.Regular);
            lblAtsPath.ForeColor = Color.FromArgb(100, 110, 120);
            grpStatus.Controls.Add(lblAtsPath);

            lblEtsStatus = new Label();
            lblEtsStatus.Location = new Point(15, 75);
            lblEtsStatus.Size = new Size(675, 22);
            lblEtsStatus.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            grpStatus.Controls.Add(lblEtsStatus);

            lblEtsPath = new Label();
            lblEtsPath.Location = new Point(32, 97);
            lblEtsPath.Size = new Size(660, 20);
            lblEtsPath.Font = new Font("Segoe UI", 8.5f, FontStyle.Regular);
            lblEtsPath.ForeColor = Color.FromArgb(100, 110, 120);
            grpStatus.Controls.Add(lblEtsPath);

            lblHint = new Label();
            lblHint.Location = new Point(15, 130);
            lblHint.Size = new Size(675, 38);
            lblHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            lblHint.ForeColor = Color.FromArgb(41, 128, 185);
            grpStatus.Controls.Add(lblHint);

            // Guide Group
            grpGuide = new GroupBox();
            grpGuide.Location = new Point(18, 238);
            grpGuide.Size = new Size(708, 205);
            grpGuide.Font = new Font("Segoe UI", 10.0f, FontStyle.Bold);
            this.Controls.Add(grpGuide);

            txtGuide = new TextBox();
            txtGuide.Multiline = true;
            txtGuide.ReadOnly = true;
            txtGuide.BackColor = Color.White;
            txtGuide.Location = new Point(15, 26);
            txtGuide.Size = new Size(678, 165);
            txtGuide.Font = new Font("Consolas", 10.0f);
            txtGuide.ScrollBars = ScrollBars.Vertical;
            grpGuide.Controls.Add(txtGuide);

            // Log Group
            grpLog = new GroupBox();
            grpLog.Location = new Point(18, 455);
            grpLog.Size = new Size(708, 265);
            grpLog.Font = new Font("Segoe UI", 10.0f, FontStyle.Bold);
            this.Controls.Add(grpLog);

            btnClearLog = new Button();
            btnClearLog.Text = "Clear";
            btnClearLog.Size = new Size(80, 26);
            btnClearLog.Location = new Point(612, 18);
            btnClearLog.Font = new Font("Segoe UI", 8.5f);
            btnClearLog.Click += (s, e) => { txtLog.Clear(); };
            grpLog.Controls.Add(btnClearLog);

            txtLog = new TextBox();
            txtLog.Multiline = true;
            txtLog.ReadOnly = true;
            txtLog.BackColor = Color.FromArgb(30, 30, 30);
            txtLog.ForeColor = Color.FromArgb(236, 240, 241);
            txtLog.Location = new Point(15, 48);
            txtLog.Size = new Size(678, 202);
            txtLog.Font = new Font("Consolas", 9.0f);
            txtLog.ScrollBars = ScrollBars.Vertical;
            grpLog.Controls.Add(txtLog);

            this.FormClosing += MainForm_FormClosing;
        }

        private void InitLanguage()
        {
            ApplyLanguage();
        }

        private void ApplyLanguage()
        {
            if (isJapanese)
            {
                grpStatus.Text = "ステータス";
                grpGuide.Text = "操作方法ガイド";
                grpLog.Text = "リアルタイム・コンソールログ";
                btnClearLog.Text = "クリア";
                lblHint.Text = "※ Steam等から通常起動してください。ゲーム終了後にこの画面を閉じるとプラグインは自動消去されます。";

                txtGuide.Text = 
                    "[F9]        : 歩行モード 切替 (ON / OFF)\r\n" +
                    "[BackSpace] : 緊急脱出 (運転席へ即座に復帰)\r\n" +
                    "[W][A][S][D]: 前後左右 移動\r\n" +
                    "[Shift]     : ダッシュ (移動速度アップ)\r\n" +
                    "[Alt]       : Noclip 切替 (壁抜け・空中浮遊・スタック脱出用)\r\n" +
                    "[Space]     : 上昇 (Noclipモード時)\r\n" +
                    "[Ctrl]      : 下降 (Noclipモード時)\r\n" +
                    "[F10]       : デバッグモニター表示 切替\r\n" +
                    "マウス移動  : 視点回転\r\n" +
                    "※隙間等にはまった場合は [Alt] のNoclipで脱出可能です。\r\n" +
                    "※駐車場の一部の背景車両にはゲーム仕様上当たり判定がありません。";
            }
            else
            {
                grpStatus.Text = "Status";
                grpGuide.Text = "Controls & Keybinds";
                grpLog.Text = "Real-time Live Console";
                btnClearLog.Text = "Clear";
                lblHint.Text = "* Launch game via Steam as usual. Plugin will be deleted automatically when closing this window.";

                txtGuide.Text = 
                    "[F9]        : Toggle Walk Mode (ON / OFF)\r\n" +
                    "[BackSpace] : Emergency Exit (Return to vehicle immediately)\r\n" +
                    "[W][A][S][D]: Move (Forward / Backward / Left / Right)\r\n" +
                    "[Shift]     : Sprint (Run faster)\r\n" +
                    "[Alt]       : Toggle Noclip (Fly, pass walls & unstuck)\r\n" +
                    "[Space]     : Ascend (In Noclip mode)\r\n" +
                    "[Ctrl]      : Descend (In Noclip mode)\r\n" +
                    "[F10]       : Toggle Debug Monitor\r\n" +
                    "Mouse       : Free Look / Turn View\r\n" +
                    "* If stuck in terrain, press [Alt] to toggle Noclip and walk out.\r\n" +
                    "* Some static parked cars in lots have no collision by game design.";
            }
            UpdateStatusLabels();
        }

        private void UpdateStatusLabels()
        {
            if (isJapanese)
            {
                if (string.IsNullOrEmpty(atsDir))
                {
                    lblAtsStatus.Text = "● ATS  : 未検知 (インストールされていません)";
                    lblAtsStatus.ForeColor = Color.Gray;
                    lblAtsPath.Text = "";
                }
                else if (atsCompatible)
                {
                    lblAtsStatus.Text = string.Format("● ATS  : 検知済み (v{0} 適合OK - 専用プラグイン有効化中)", atsVersion);
                    lblAtsStatus.ForeColor = Color.FromArgb(39, 174, 96);
                    lblAtsPath.Text = atsDir;
                }
                else
                {
                    lblAtsStatus.Text = string.Format("▲ ATS  : 検知済み (v{0} - 未対応バージョンのため安全停止中)", atsVersion);
                    lblAtsStatus.ForeColor = Color.FromArgb(230, 126, 34);
                    lblAtsPath.Text = atsDir;
                }

                if (string.IsNullOrEmpty(etsDir))
                {
                    lblEtsStatus.Text = "● ETS2 : 未検知 (インストールされていません)";
                    lblEtsStatus.ForeColor = Color.Gray;
                    lblEtsPath.Text = "";
                }
                else if (etsCompatible)
                {
                    lblEtsStatus.Text = string.Format("● ETS2 : 検知済み (v{0} 適合OK - 専用プラグイン有効化中)", etsVersion);
                    lblEtsStatus.ForeColor = Color.FromArgb(39, 174, 96);
                    lblEtsPath.Text = etsDir;
                }
                else
                {
                    lblEtsStatus.Text = string.Format("▲ ETS2 : 検知済み (v{0} - 未対応バージョンのため安全停止中)", etsVersion);
                    lblEtsStatus.ForeColor = Color.FromArgb(230, 126, 34);
                    lblEtsPath.Text = etsDir;
                }
            }
            else
            {
                if (string.IsNullOrEmpty(atsDir))
                {
                    lblAtsStatus.Text = "● ATS  : Not Detected";
                    lblAtsStatus.ForeColor = Color.Gray;
                    lblAtsPath.Text = "";
                }
                else if (atsCompatible)
                {
                    lblAtsStatus.Text = string.Format("● ATS  : Detected (v{0} Compatible - Plugin Active)", atsVersion);
                    lblAtsStatus.ForeColor = Color.FromArgb(39, 174, 96);
                    lblAtsPath.Text = atsDir;
                }
                else
                {
                    lblAtsStatus.Text = string.Format("▲ ATS  : Detected (v{0} - Unsupported Version, Paused)", atsVersion);
                    lblAtsStatus.ForeColor = Color.FromArgb(230, 126, 34);
                    lblAtsPath.Text = atsDir;
                }

                if (string.IsNullOrEmpty(etsDir))
                {
                    lblEtsStatus.Text = "● ETS2 : Not Detected";
                    lblEtsStatus.ForeColor = Color.Gray;
                    lblEtsPath.Text = "";
                }
                else if (etsCompatible)
                {
                    lblEtsStatus.Text = string.Format("● ETS2 : Detected (v{0} Compatible - Plugin Active)", etsVersion);
                    lblEtsStatus.ForeColor = Color.FromArgb(39, 174, 96);
                    lblEtsPath.Text = etsDir;
                }
                else
                {
                    lblEtsStatus.Text = string.Format("▲ ETS2 : Detected (v{0} - Unsupported Version, Paused)", etsVersion);
                    lblEtsStatus.ForeColor = Color.FromArgb(230, 126, 34);
                    lblEtsPath.Text = etsDir;
                }
            }
        }

        private void AppendLog(string message)
        {
            string timeStr = DateTime.Now.ToString("HH:mm:ss");
            txtLog.AppendText(string.Format("[{0}] {1}\r\n", timeStr, message));
        }

        private void EnsureGameConfigDevMode(string gameFolderName, string gameDisplayName)
        {
            try
            {
                List<string> candidateDirs = new List<string>();
                string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
                if (!string.IsNullOrEmpty(docs)) candidateDirs.Add(docs);

                string userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
                if (!string.IsNullOrEmpty(userProfile))
                {
                    candidateDirs.Add(Path.Combine(userProfile, "Documents"));
                    candidateDirs.Add(Path.Combine(userProfile, "OneDrive", "Documents"));
                    candidateDirs.Add(Path.Combine(userProfile, "OneDrive", @"ドキュメント"));
                }

                foreach (string cDir in candidateDirs)
                {
                    string cfgPath = Path.Combine(cDir, gameFolderName, "config.cfg");
                    if (File.Exists(cfgPath))
                    {
                        bool modified = false;
                        string[] lines = File.ReadAllLines(cfgPath);
                        for (int i = 0; i < lines.Length; i++)
                        {
                            if (Regex.IsMatch(lines[i], @"^\s*uset\s+g_developer\s+""0"""))
                            {
                                lines[i] = "uset g_developer \"1\"";
                                modified = true;
                            }
                            else if (Regex.IsMatch(lines[i], @"^\s*uset\s+g_console\s+""0"""))
                            {
                                lines[i] = "uset g_console \"1\"";
                                modified = true;
                            }
                        }

                        if (modified)
                        {
                            File.WriteAllLines(cfgPath, lines);
                            AppendLog(string.Format("[{0}] config.cfg の g_developer / g_console を \"1\" に自動設定しました。", gameDisplayName));
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                AppendLog(string.Format("[{0}] config.cfg 自動設定エラー: {1}", gameDisplayName, ex.Message));
            }
        }

        private void DetectGamesAndInstall()
        {
            atsDir = DetectSteamGamePath("270880", "American Truck Simulator");
            etsDir = DetectSteamGamePath("227300", "Euro Truck Simulator 2");

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string atsSourceDll = Path.Combine(baseDir, "TruckerWalkAround_ATS.dll");
            if (!File.Exists(atsSourceDll)) atsSourceDll = Path.Combine(baseDir, "TruckerWalkAround.dll");

            string etsSourceDll = Path.Combine(baseDir, "TruckerWalkAround_ETS2.dll");
            if (!File.Exists(etsSourceDll)) etsSourceDll = Path.Combine(baseDir, "TruckerWalkAround.dll");

            // ATS バージョン検査 & 設置
            if (!string.IsNullOrEmpty(atsDir))
            {
                string exePath = Path.Combine(atsDir, "bin", "win_x64", "amtrucks.exe");
                atsVersion = GetGameExeVersion(exePath);
                atsCompatible = CheckVersionCompatibility(atsVersion);

                AppendLog(string.Format("American Truck Simulator を検知しました (バージョン: {0})", atsVersion));
                EnsureGameConfigDevMode("American Truck Simulator", "ATS");
                if (atsCompatible && File.Exists(atsSourceDll))
                {
                    InstallDllToGame(atsDir, atsSourceDll, "ATS");
                }
                else if (!atsCompatible)
                {
                    AppendLog(string.Format("[警告] ATS のバージョン ({0}) はサポート対象外です。ゲームクラッシュ防止のためプラグイン配置を停止しました。", atsVersion));
                }
                else
                {
                    AppendLog("ERROR: TruckerWalkAround_ATS.dll が見つかりません。");
                }
            }

            // ETS2 バージョン検査 & 設置
            if (!string.IsNullOrEmpty(etsDir))
            {
                string exePath = Path.Combine(etsDir, "bin", "win_x64", "eurotrucks2.exe");
                etsVersion = GetGameExeVersion(exePath);
                etsCompatible = CheckVersionCompatibility(etsVersion);

                AppendLog(string.Format("Euro Truck Simulator 2 を検知しました (バージョン: {0})", etsVersion));
                EnsureGameConfigDevMode("Euro Truck Simulator 2", "ETS2");
                if (etsCompatible && File.Exists(etsSourceDll))
                {
                    InstallDllToGame(etsDir, etsSourceDll, "ETS2");
                }
                else if (!etsCompatible)
                {
                    AppendLog(string.Format("[警告] ETS2 のバージョン ({0}) はサポート対象外です。ゲームクラッシュ防止のためプラグイン配置を停止しました。", etsVersion));
                }
                else
                {
                    AppendLog("ERROR: TruckerWalkAround_ETS2.dll が見つかりません。");
                }
            }

            if (string.IsNullOrEmpty(atsDir) && string.IsNullOrEmpty(etsDir))
            {
                AppendLog("警告: ATS / ETS2 のインストールフォルダが検出できませんでした。");
            }
            else if (atsCompatible || etsCompatible)
            {
                AppendLog("対応ゲームへの専用プラグイン設置が完了しました。Steamからゲームを起動してください。");
            }
            UpdateStatusLabels();
        }

        private string GetGameExeVersion(string exePath)
        {
            try
            {
                if (File.Exists(exePath))
                {
                    FileVersionInfo info = FileVersionInfo.GetVersionInfo(exePath);
                    if (!string.IsNullOrEmpty(info.ProductVersion))
                    {
                        string ver = info.ProductVersion.Trim();
                        int spaceIdx = ver.IndexOf(' ');
                        if (spaceIdx > 0) ver = ver.Substring(0, spaceIdx);
                        return ver;
                    }
                    if (!string.IsNullOrEmpty(info.FileVersion))
                    {
                        string ver = info.FileVersion.Trim();
                        int spaceIdx = ver.IndexOf(' ');
                        if (spaceIdx > 0) ver = ver.Substring(0, spaceIdx);
                        return ver;
                    }
                }
            }
            catch { }
            return "Unknown";
        }

        private bool CheckVersionCompatibility(string gameVersion)
        {
            if (string.IsNullOrEmpty(gameVersion) || gameVersion == "Unknown") return false;

            foreach (string supported in SupportedMajorVersions)
            {
                if (gameVersion.StartsWith(supported))
                {
                    return true;
                }
            }
            return false;
        }

        private void InstallDllToGame(string gameRoot, string sourceDll, string gameName)
        {
            try
            {
                string pluginsDir = Path.Combine(gameRoot, "bin", "win_x64", "plugins");
                if (!Directory.Exists(pluginsDir))
                {
                    Directory.CreateDirectory(pluginsDir);
                }
                // ゲーム内では共通の TruckerWalkAround.dll として読み込ませる
                string destDll = Path.Combine(pluginsDir, "TruckerWalkAround.dll");
                File.Copy(sourceDll, destDll, true);
                installedDllPaths.Add(destDll);
                AppendLog(string.Format("[{0}] 専用プラグインを設置しました ({1} -> {2})", gameName, Path.GetFileName(sourceDll), destDll));
            }
            catch (Exception ex)
            {
                AppendLog(string.Format("[{0}] プラグイン設置失敗: {1}", gameName, ex.Message));
            }
        }

        private string DetectSteamGamePath(string appId, string defaultSubDir)
        {
            try
            {
                string keyPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App " + appId;
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(keyPath))
                {
                    if (key != null)
                    {
                        object loc = key.GetValue("InstallLocation");
                        if (loc != null && Directory.Exists(loc.ToString()))
                        {
                            return loc.ToString();
                        }
                    }
                }
            }
            catch { }

            try
            {
                string steamPath = "";
                using (RegistryKey sKey = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    if (sKey != null)
                    {
                        object p = sKey.GetValue("SteamPath");
                        if (p != null) steamPath = p.ToString().Replace('/', '\\');
                    }
                }

                if (!string.IsNullOrEmpty(steamPath) && Directory.Exists(steamPath))
                {
                    string defaultGame = Path.Combine(steamPath, "steamapps", "common", defaultSubDir);
                    if (Directory.Exists(defaultGame)) return defaultGame;

                    string vdfPath = Path.Combine(steamPath, "steamapps", "libraryfolders.vdf");
                    if (File.Exists(vdfPath))
                    {
                        string[] lines = File.ReadAllLines(vdfPath);
                        foreach (string line in lines)
                        {
                            Match m = Regex.Match(line, "\"path\"\\s+\"([^\"]+)\"");
                            if (m.Success)
                            {
                                string libPath = m.Groups[1].Value.Replace(@"\\", @"\");
                                string candidate = Path.Combine(libPath, "steamapps", "common", defaultSubDir);
                                if (Directory.Exists(candidate)) return candidate;
                            }
                        }
                    }
                }
            }
            catch { }

            return "";
        }

        private void InitLogWatcher()
        {
            string temp = Path.GetTempPath();
            logFilePath = Path.Combine(temp, "TruckerWalkAround_Live.log");

            try
            {
                if (File.Exists(logFilePath))
                {
                    File.Delete(logFilePath);
                }
            }
            catch { }

            lastLogPosition = 0;
            logTimer = new System.Windows.Forms.Timer();
            logTimer.Interval = 250;
            logTimer.Tick += LogTimer_Tick;
            logTimer.Start();
        }

        private void LogTimer_Tick(object sender, EventArgs e)
        {
            if (!File.Exists(logFilePath)) return;

            try
            {
                using (FileStream fs = new FileStream(logFilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    if (fs.Length > lastLogPosition)
                    {
                        fs.Seek(lastLogPosition, SeekOrigin.Begin);
                        using (StreamReader reader = new StreamReader(fs, Encoding.UTF8))
                        {
                            string newContent = reader.ReadToEnd();
                            lastLogPosition = fs.Position;
                            if (!string.IsNullOrEmpty(newContent))
                            {
                                txtLog.AppendText(newContent);
                            }
                        }
                    }
                    else if (fs.Length < lastLogPosition)
                    {
                        lastLogPosition = 0;
                    }
                }
            }
            catch { }
        }

        private void InitProcessMonitor()
        {
            processMonitorTimer = new System.Windows.Forms.Timer();
            processMonitorTimer.Interval = 1000;
            processMonitorTimer.Tick += ProcessMonitorTimer_Tick;
            processMonitorTimer.Start();
        }

        private void ProcessMonitorTimer_Tick(object sender, EventArgs e)
        {
            Process[] procs = GetGameProcesses();
            bool isRunning = (procs.Length > 0);

            if (isRunning)
            {
                if (!wasGameRunning)
                {
                    wasGameRunning = true;
                    AppendLog(isJapanese ? "ゲームプロセスを検知しました (実行中)。" : "Game process detected (Running).");
                }
            }
            else
            {
                if (wasGameRunning)
                {
                    wasGameRunning = false;
                    AppendLog(isJapanese ? "ゲームが正常に終了しました。" : "Game exited.");
                }
            }
        }

        private Process[] GetGameProcesses()
        {
            List<Process> list = new List<Process>();
            list.AddRange(Process.GetProcessesByName("amtrucks"));
            list.AddRange(Process.GetProcessesByName("eurotrucks2"));
            return list.ToArray();
        }

        private void MainForm_FormClosing(object sender, FormClosingEventArgs e)
        {
            if (logTimer != null) logTimer.Stop();
            if (processMonitorTimer != null) processMonitorTimer.Stop();

            Process[] procs = GetGameProcesses();
            if (procs.Length > 0)
            {
                string msg = isJapanese 
                    ? "ゲームが起動中です。ゲームを終了してからマネージャーを閉じるか、強制終了してプラグインを安全に削除しますか？\r\n(「はい」を押すと強制終了してプラグインを削除します)"
                    : "The game is currently running. Do you want to close the game and clean up plugins?";
                if (MessageBox.Show(msg, "TruckerWalkAround", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                {
                    foreach (Process p in procs)
                    {
                        try { p.Kill(); p.WaitForExit(2000); } catch { }
                    }
                }
                else
                {
                    e.Cancel = true;
                    if (logTimer != null) logTimer.Start();
                    if (processMonitorTimer != null) processMonitorTimer.Start();
                    return;
                }
            }

            // Remove plugins
            foreach (string dllPath in installedDllPaths)
            {
                try
                {
                    if (File.Exists(dllPath))
                    {
                        File.Delete(dllPath);
                    }
                }
                catch { }
            }
        }

        [STAThread]
        public static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
