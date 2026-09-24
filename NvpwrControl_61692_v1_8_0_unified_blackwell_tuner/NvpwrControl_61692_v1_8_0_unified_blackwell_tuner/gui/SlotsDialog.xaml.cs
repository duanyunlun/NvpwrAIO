using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace NvpwrControl
{
    /// <summary>
    /// Slot picker.
    ///
    /// One combo selects the slot; the name box above the buttons renames it. "New
    /// slot" is therefore just picking an empty one and typing a name — there is no
    /// separate create step, because a slot is only a name plus a snapshot and an
    /// unused slot is already conceptually empty.
    ///
    /// The dialog edits the caller's array in place and the main window persists it on
    /// close, so there is exactly one owner of the record.
    /// </summary>
    public partial class SlotsDialog : Window
    {
        private readonly MainWindow _owner;
        private readonly ConfigSlot[] _slots;
        private bool _loading;   // suppresses SelectionChanged while the list is rebuilt

        internal SlotsDialog(MainWindow owner, ConfigSlot[] slots)
        {
            InitializeComponent();
            _owner = owner;
            _slots = slots;

            RebuildCombo(0);
        }

        private void RebuildCombo(int select)
        {
            _loading = true;
            SlotCombo.Items.Clear();
            for (int i = 0; i < Store.SlotCount; i++)
            {
                ConfigSlot s = _slots[i];
                string label = s.Used
                    ? ((i + 1) + ". " + s.Name)
                    : ((i + 1) + ". （空槽位）");
                SlotCombo.Items.Add(label);
            }
            SlotCombo.SelectedIndex = Math.Max(0, Math.Min(Store.SlotCount - 1, select));
            _loading = false;

            ShowSelected();
        }

        private void OnSlotChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            ShowSelected();
        }

        private int Selected
        {
            get { return SlotCombo.SelectedIndex < 0 ? 0 : SlotCombo.SelectedIndex; }
        }

        private void ShowSelected()
        {
            ConfigSlot s = _slots[Selected];

            SlotName.Text = s.Used ? s.Name : "";

            if (!s.Used)
            {
                SlotDetail.Text = "空槽位。填一个名称后点“保存到此槽位”。";
                return;
            }

            StringBuilder sb = new StringBuilder();
            sb.Append("保存时间：").Append(s.SavedAt);

            DesiredState st = s.State;
            sb.Append("\n功耗：");
            if (st != null && st.PowerEnabled && st.PowerMw != 0)
                sb.Append(st.PowerMw / 1000).Append(" W");
            else sb.Append("出厂上限");

            sb.Append("\n电压：核心轨 ");
            long rel = (st != null) ? st.Voltage.Nvvdd.RelUv / 1000 : 0;
            sb.Append(rel > 0 ? "+" : "").Append(rel).Append(" mV");

            sb.Append("\n频率：核心 ");
            long core = (st != null) ? st.Clock.CoreOffsetMhz : 0;
            long mem = (st != null) ? st.Clock.MemoryOffsetMhz : 0;
            sb.Append(core > 0 ? "+" : "").Append(core).Append(" MHz，显存 ");
            sb.Append(mem > 0 ? "+" : "").Append(mem).Append(" MHz");

            if (s.LastApplyOk) sb.Append("\n\n上次应用：成功");
            else if (!string.IsNullOrEmpty(s.LastApplyNote)) sb.Append("\n\n上次应用：失败 —— ").Append(s.LastApplyNote);

            SlotDetail.Text = sb.ToString();
        }

        private void OnSave(object sender, RoutedEventArgs e)
        {
            int index = Selected;
            ConfigSlot s = _slots[index];

            string name = SlotName.Text.Trim();
            if (string.IsNullOrEmpty(name)) name = "槽位 " + (index + 1);

            s.Used = true;
            s.Name = name;
            s.SavedAt = DateTime.Now.ToString("yyyy-MM-dd HH:mm");

            // Snapshot the live state plus whatever the main window has staged but not
            // yet applied, so saving while a change is pending records what the user
            // sees rather than only what is already live.
            s.State = _owner.State.Clone();
            s.State.PowerEnabled = _owner.TargetWatts != 0;
            s.State.PowerMw = (uint)_owner.TargetWatts * 1000u;
            // Every voltage control, through the owner's helper: writing Nvvdd by hand with only
            // REL in it discarded VMIN, ALT/OP and OV, so a slot could not reproduce a full
            // voltage configuration.
            _owner.WritePendingVoltage(s.State);

            s.LastApplyOk = false;
            s.LastApplyNote = "";

            RebuildCombo(index);
            Store.Log("已保存槽位 " + (index + 1) + " (" + name + ")");
        }

        private void OnLoad(object sender, RoutedEventArgs e)
        {
            int index = Selected;
            ConfigSlot s = _slots[index];
            if (!s.Used) { _owner.Info("该槽位是空的。"); return; }
            if (!_owner.Confirm("载入槽位“" + s.Name + "”并立即应用？")) return;

            bool ok;
            List<string> failures;
            _owner.ApplySlot(s, out ok, out failures);

            s.LastApplyOk = ok;
            s.LastApplyNote = ok ? "" : string.Join("；", failures.ToArray());
            RebuildCombo(index);

            if (!ok)
                _owner.Warn("槽位“" + s.Name + "”部分环节失败：\r\n\r\n" + string.Join("\r\n", failures.ToArray()));
        }

        private void OnDelete(object sender, RoutedEventArgs e)
        {
            int index = Selected;
            ConfigSlot s = _slots[index];
            if (!s.Used) { _owner.Info("该槽位已经是空的。"); return; }
            if (!_owner.Confirm("删除槽位“" + s.Name + "”？")) return;

            _slots[index] = new ConfigSlot();
            RebuildCombo(index);
            Store.Log("已删除槽位 " + (index + 1));
        }

        private void OnRestoreDefaults(object sender, RoutedEventArgs e)
        {
            _owner.RestoreDefaults();
            RebuildCombo(Selected);
        }

        private void OnClose(object sender, RoutedEventArgs e)
        {
            Close();
        }
    }
}
