using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

namespace NvpwrControl
{
    /// <summary>
    /// EfiGuard installation: detection, file placement, and the one step that has to be done
    /// by hand.
    ///
    /// Why this is not a one-click installer. EfiGuard has to be reached before Windows starts,
    /// which means the firmware has to hold a boot entry pointing at its loader. Creating that
    /// entry turns out to be impossible from Windows:
    ///
    ///   bcdedit only creates BOOTAPP, BOOTSECTOR, OSLOADER, RESUME and STARTUP entries. None of
    ///   them appear in the firmware boot manager.
    ///
    ///   Writing the UEFI variables directly fails with 1314 ERROR_PRIVILEGE_NOT_HELD, reads
    ///   included, with SeSystemEnvironmentPrivilege enabled. Windows has blocked user-mode
    ///   access to firmware variables since Windows 8.
    ///
    /// So the entry comes from the firmware itself, which enumerates partitions and registers any
    /// that carry \EFI\BOOT\BOOTX64.EFI — the removable-media fallback path. On the reference
    /// machine it appeared as "UEFI OS" with no prompting, and the user moved it to the front of
    /// the boot order in the BIOS.
    ///
    /// That is the split this class works with: everything up to "the files are in place" is
    /// automatic, and the firmware step is described rather than attempted.
    ///
    /// A separate partition is used rather than the Windows ESP. The ESP already has
    /// \EFI\BOOT\bootx64.efi, and on the reference machine that is a 3 MB copy of the real boot
    /// manager; overwriting it to get EfiGuard auto-detected risks the machine's own boot path.
    /// </summary>
    internal static class EfiGuardSetup
    {
        internal sealed class Status
        {
            /// <summary>Drive root of the partition holding the loader, e.g. "Z:\".</summary>
            public string PartitionRoot;
            public bool PartitionFound { get { return PartitionRoot != null; } }

            /// <summary>Both files are present where the firmware looks for them.</summary>
            public bool FilesPresent;

            /// <summary>The firmware boot manager lists an entry for the loader.</summary>
            public bool EntryFound;

            /// <summary>That entry is first in the firmware boot order.</summary>
            public bool EntryFirst;

            /// <summary>This boot went through EfiGuard, i.e. the loader ran and patched.</summary>
            public bool ActiveThisBoot;

            public string Note = "";

            /// <summary>Everything needed for the driver to load is in place.</summary>
            public bool Ready { get { return FilesPresent && EntryFound && EntryFirst && ActiveThisBoot; } }
        }

        /// <summary>Where the firmware looks for a fallback loader on a partition.</summary>
        private const string BootDir = @"EFI\BOOT";
        private const string LoaderName = "BOOTX64.EFI";
        private const string DxeName = "EfiGuardDxe.efi";
        private const string LabelHint = "EFIGUARD";

        /// <summary>
        /// The partition's volume label, as an extra hint. Not required: the reference machine's
        /// partition is labelled EFIGUARD because that is what the instructions said to do, but a
        /// partition that merely has the files in the right place is just as usable.
        /// </summary>
        public static Status Detect()
        {
            Status s = new Status();
            s.ActiveThisBoot = DseControl.IsAvailable() && DseControl.IsBooted();

            // Look for the files first: a partition that already carries them is the answer
            // regardless of what it is labelled.
            foreach (DriveInfo d in SafeDrives())
            {
                if (!d.IsReady) continue;
                string boot;
                string dxe;
                if (!TryPaths(d.RootDirectory.FullName, out boot, out dxe)) continue;
                bool b = File.Exists(boot);
                bool x = File.Exists(dxe);
                if (b || x)
                {
                    Store.Log("EfiGuard 分区扫描: " + d.RootDirectory.FullName +
                              " boot=" + b + " dxe=" + x);
                }
                if (b && x)
                {
                    s.PartitionRoot = d.RootDirectory.FullName;
                    s.FilesPresent = true;
                    break;
                }
            }

            // Otherwise fall back to the label, so a half-finished install is still recognised
            // and can be completed rather than started from scratch.
            if (s.PartitionRoot == null)
            {
                foreach (DriveInfo d in SafeDrives())
                {
                    if (!d.IsReady) continue;
                    string label = "";
                    try { label = d.VolumeLabel ?? ""; } catch { }
                    if (label.Trim().Equals(LabelHint, StringComparison.OrdinalIgnoreCase))
                    {
                        s.PartitionRoot = d.RootDirectory.FullName;
                        break;
                    }
                }
            }

            ParseFirmware(s);

            Store.Log("EfiGuard 检测: 分区=" + (s.PartitionRoot ?? "(无)") +
                      " 文件=" + s.FilesPresent +
                      " 启动项=" + s.EntryFound +
                      " 第一=" + s.EntryFirst +
                      " 已生效=" + s.ActiveThisBoot);
            return s;
        }

        /// <summary>
        /// One or two words for the chip. Nothing more.
        ///
        /// The chip already carries the state in its colour, and says whether it can be acted
        /// on by being clickable, so spelling either of those out beside the name only makes the
        /// row longer than it needs to be. What the state means, why it is that way, and what to
        /// do about it all live in the tooltip.
        /// </summary>
        public static string Describe(Status s)
        {
            if (s.ActiveThisBoot) return "已生效";
            if (!s.PartitionFound || !s.FilesPresent) return "未安装";
            return "未生效";
        }

        /// <summary>
        /// The full picture, for the chip's tooltip.
        ///
        /// Everything the installer used to do is described here rather than performed. Two
        /// findings settled that, and both were measured on the reference machine:
        ///
        ///   Removing the loader file does stop EfiGuard from running — the firmware tries the
        ///   entry, fails to load it, and moves on to Windows. But it also drops the entry, and
        ///   putting the file back does not put the entry back into the boot order. A toggle that
        ///   works in one direction only is a trap in something that looks like a switch.
        ///
        ///   Reordering with bcdedit reports success and changes what bcdedit shows, and the
        ///   firmware ignores it, because the order it actually boots from is its own. Writing the
        ///   UEFI variables directly is refused with 1314 even with SeSystemEnvironmentPrivilege.
        ///
        /// So the boot order belongs to the user and the BIOS, and this says what to do about it.
        /// </summary>
        public static string Hint(Status s)
        {
            StringBuilder b = new StringBuilder();
            b.AppendLine("EfiGuard 在启动阶段修补内核，让本程序可以临时关闭驱动签名强制。");
            b.AppendLine();
            b.AppendLine("当前状态");
            b.AppendLine("  分区       " + (s.PartitionFound ? s.PartitionRoot : "未找到"));
            b.AppendLine("  引导文件   " + (s.FilesPresent ? "已就位" : "缺失"));
            b.AppendLine("  固件启动项 " + (s.EntryFound
                ? (s.EntryFirst ? "已登记，且是第一启动项" : "已登记，但不在第一位")
                : "未登记"));
            b.AppendLine("  本次生效   " + (s.ActiveThisBoot ? "是" : "否"));
            b.AppendLine();

            if (s.ActiveThisBoot)
            {
                b.AppendLine("一切就绪，无需操作。");
                return b.ToString();
            }

            b.AppendLine("需要手动处理：");
            if (!s.PartitionFound || !s.FilesPresent)
            {
                b.AppendLine("  准备一个约 50 MB 的 FAT32 分区，把 bootx64.efi 和");
                b.AppendLine("  EfiGuardDxe.efi 放进它的 \\EFI\\BOOT\\ 目录，然后重启。");
            }
            else if (!s.EntryFound)
            {
                b.AppendLine("  重启一次，让固件发现该分区并登记「UEFI OS」启动项。");
            }
            else if (!s.EntryFirst)
            {
                b.AppendLine("  开机时进 BIOS，把「UEFI OS」调到启动顺序第一位。");
                b.AppendLine("  它在固件里的名字是通用的「UEFI OS」，不是「EfiGuard」。");
            }
            else
            {
                b.AppendLine("  启动顺序已正确，重启后即会生效。");
            }
            b.AppendLine();
            b.AppendLine("程序无法代劳：启动顺序存在主板固件里。bcdedit 的修改会被固件");
            b.AppendLine("在启动时覆盖，直接写固件变量也会被 Windows 拒绝（错误码 1314）。");
            return b.ToString();
        }

        // ------------------------------------------------------------------ helpers

        private static bool TryPaths(string root, out string boot, out string dxe)
        {
            boot = null;
            dxe = null;
            try
            {
                boot = Path.Combine(root, BootDir, LoaderName);
                dxe = Path.Combine(root, BootDir, DxeName);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static IEnumerable<DriveInfo> SafeDrives()
        {
            try { return DriveInfo.GetDrives(); }
            catch { return new DriveInfo[0]; }
        }

        /// <summary>
        /// Reads the firmware boot manager out of bcdedit: whether an entry points at the
        /// loader, and whether it comes first.
        ///
        /// Parsed rather than queried, because there is no interface for this that works from
        /// user mode. The output is a sequence of blocks, each a header line underlined with
        /// dashes:
        ///
        ///   Firmware Boot Manager
        ///   ---------------------
        ///   identifier              {a5a30fa2-...}
        ///   displayorder            {0eb8d7ba-...}
        ///                           {9dea862c-...}
        ///
        ///   Firmware Application (101fffff)
        ///   -------------------------------
        ///   identifier              {0eb8d7ba-...}
        ///   device                  partition=Z:
        ///   path                    \EFI\BOOT\BOOTX64.EFI
        ///   description             UEFI OS
        ///
        /// displayorder spills onto continuation lines that carry no key, which is the only
        /// awkward part: those lines are indented, and that is what distinguishes them.
        /// </summary>
        private static void ParseFirmware(Status s)
        {
            string output = RunBcd("/enum firmware");
            if (string.IsNullOrEmpty(output)) return;

            string[] lines = output.Replace("\r", "").Split('\n');
            List<Block> blocks = ParseBlocks(lines);

            /*
                Matched on the identifier value rather than on the headings.

                bcdedit localises its output. On this machine the same command prints
                固件启动管理器 and 标识符 from the application, while a shell session got the
                English words — and the parser, which looked for "Firmware Boot Manager" and
                "identifier", found nothing at all. The element names path, device and
                displayorder are left untranslated, so the values are what can be relied on.
            */
            string firstEntry = null;
            foreach (Block b in blocks)
            {
                if (b.Id != null &&
                    b.Id.Equals("{fwbootmgr}", StringComparison.OrdinalIgnoreCase) &&
                    b.DisplayOrder.Count > 0)
                {
                    firstEntry = b.DisplayOrder[0];
                    break;
                }
            }

            string loaderId = null;
            foreach (Block b in blocks)
            {
                if (b.Path != null &&
                    b.Path.Equals(@"\EFI\BOOT\BOOTX64.EFI", StringComparison.OrdinalIgnoreCase))
                {
                    s.EntryFound = true;
                    loaderId = b.Id;
                    break;
                }
            }

            if (s.EntryFound && firstEntry != null && loaderId != null)
            {
                s.EntryFirst = firstEntry.Equals(loaderId, StringComparison.OrdinalIgnoreCase);
            }
        }

        /// <summary>One entry in the bcdedit output.</summary>
        private sealed class Block
        {
            /// <summary>
            /// The entry's identifier: the value of the first key/value pair in the block,
            /// whatever the key happens to be called in this locale.
            /// </summary>
            public string Id;
            public string Path;

            /// <summary>Every value on the displayorder line, continuation lines included.</summary>
            public readonly List<string> DisplayOrder = new List<string>();
        }

        /// <summary>
        /// Splits the output into blocks.
        ///
        /// The format is a heading underlined with dashes, then key/value lines, with
        /// displayorder spilling onto continuation lines that carry no key. The identifier is
        /// taken to be the first key/value pair in a block, which is the order bcdedit emits and
        /// avoids depending on what the key is called.
        /// </summary>
        private static List<Block> ParseBlocks(string[] lines)
        {
            List<Block> blocks = new List<Block>();
            Block cur = null;

            for (int i = 0; i < lines.Length; i++)
            {
                string trimmed = lines[i].Trim();

                bool nextIsDashes = i + 1 < lines.Length &&
                    lines[i + 1].TrimStart().StartsWith("---", StringComparison.Ordinal);
                if (trimmed.Length > 0 && nextIsDashes)
                {
                    cur = new Block();
                    blocks.Add(cur);
                    i++;   // skip the underline
                    continue;
                }
                if (trimmed.Length == 0 || cur == null) continue;

                string key, value;
                SplitKeyValue(lines[i], out key, out value);

                // A continuation line of displayorder: no key, and the value is an identifier.
                if (key.Length == 0)
                {
                    if (value.StartsWith("{", StringComparison.Ordinal) && cur.DisplayOrder.Count > 0)
                    {
                        cur.DisplayOrder.Add(value);
                    }
                    continue;
                }

                if (cur.Id == null)
                {
                    cur.Id = value;
                    continue;
                }
                if (key.Equals("path", StringComparison.OrdinalIgnoreCase))
                {
                    cur.Path = value;
                    continue;
                }
                if (key.Equals("displayorder", StringComparison.OrdinalIgnoreCase))
                {
                    if (value.Length > 0) cur.DisplayOrder.Add(value);
                    continue;
                }
            }
            return blocks;
        }

        /// <summary>
        /// Splits a bcdedit line into its key and value.
        ///
        /// The separator is a run of spaces, and lines that are themselves indented are
        /// continuations of the previous key rather than new ones.
        /// </summary>
        private static void SplitKeyValue(string raw, out string key, out string value)
        {
            key = "";
            value = "";
            int lead = 0;
            while (lead < raw.Length && raw[lead] == ' ') lead++;

            if (lead > 0)
            {
                value = raw.Trim();
                return;
            }
            int gap = raw.IndexOf("  ", StringComparison.Ordinal);
            if (gap < 0)
            {
                key = raw.Trim();
                return;
            }
            key = raw.Substring(0, gap).Trim();
            value = raw.Substring(gap).Trim();
        }

        private static string RunBcd(string arguments)
        {
            // Resolved against System32 rather than left to PATH. A GUI process started from
            // Explorer does inherit the system PATH, but the search order also includes the
            // application directory, and "bcdedit" is a name worth being certain about.
            string exe;
            try
            {
                exe = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.System), "bcdedit.exe");
                if (!File.Exists(exe)) exe = "bcdedit";
            }
            catch { exe = "bcdedit"; }

            try
            {
                ProcessStartInfo psi = new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = arguments,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (Process p = Process.Start(psi))
                {
                    if (p == null) return null;
                    string so = p.StandardOutput.ReadToEnd();
                    string se = p.StandardError.ReadToEnd();
                    p.WaitForExit(20000);
                    if (so.Length == 0 && se.Length > 0)
                    {
                        Store.Log("EfiGuard: bcdedit " + arguments + " 失败: " + se.Trim());
                    }
                    return so;
                }
            }
            catch (Exception ex)
            {
                Store.Log("EfiGuard: bcdedit " + arguments + " 异常: " + ex.Message);
                return null;
            }
        }
    }
}