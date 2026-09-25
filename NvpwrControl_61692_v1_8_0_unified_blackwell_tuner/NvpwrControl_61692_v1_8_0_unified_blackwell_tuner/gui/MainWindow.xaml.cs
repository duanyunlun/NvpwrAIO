using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace NvpwrControl
{
	public partial class MainWindow : Window
	{
		private struct RECT
		{
			public int Left;

			public int Top;

			public int Right;

			public int Bottom;
		}

		private const int POWER_STEP_W = 25;

		private const int POWER_MAX_W = 350;

		private const int VOLT_STEP_MV = 5;

		/// <summary>
		/// Step for the clock offset steppers, matching the power card coarse steps. The middle
		/// field is typed as well, so this is for nudging rather than for getting there.
		/// </summary>
		/// <summary>
		/// The NVIDIA driver build this program is written against.
		///
		/// Not a preference. The kernel helper reads and writes the power policy at offsets taken
		/// from this build's internal layout — which is why the driver carries an exact-build guard
		/// of its own — so on any other build it would be writing into whatever sits at those
		/// offsets. The version is shown in the header and turns red when it does not match, so the
		/// mismatch is visible before anything is applied rather than discovered as a crash.
		/// </summary>
		private const string SUPPORTED_DRIVER = "616.92";

		private const int CLK_STEP_MHZ = 50;

		private const int VOLT_MIN_MV = -25;

		private const int VOLT_MAX_MV = 50;

		private const int LIMIT_MIN_MV = -50;

		private const int LIMIT_MAX_MV = 50;

		private const int OV_BASE_MV = 1200;

		/// <summary>
		/// Lower end of the absolute voltage sliders, taken from nvvdd_device_range_uv. Used as a
		/// floor when clamping the minimum, so the slider can never be squeezed below the smallest
		/// voltage the rail reports.
		/// </summary>
		private const int ABS_MIN_MV = 445;

		private int _absMinMv = 445;

		private int _absMaxMv = 1280;

		private DesiredState _state = DesiredState.Default();

		private ConfigSlot[] _slots = new ConfigSlot[6];

		private uint _profile;

		private string _gpuName = "检测中…";

		private string _mvoltPath = "";

		private bool _mvoltReady;

		private int _targetW;

		private int _powerFloorW = 175;

		private int _voltOffsetMv;

		private int _xbarOffsetMv;

		private int _vminOffsetMv;

		private int _relOffsetMv;

		private int _altOffsetMv;

		private int _ovOffsetMv;

		private int _baseMinMv = 625;

		private int _baseMaxMv = 1025;

		private int _appliedMinMv = 625;

		private int _appliedMaxMv = 1025;

		private SettingsDialog _settingsDialog;

		private SlotsDialog _slotsDialog;

		private DispatcherTimer _refreshTimer;



		private const double DESIGN_CLIENT_W = 1280.0;

		private const double DESIGN_CLIENT_H = 720.0;


		internal DesiredState State => _state;

		internal int TargetWatts => _targetW;

		internal int VoltOffsetMv => _voltOffsetMv;

		internal int XbarOffsetMv => _xbarOffsetMv;

		internal int RelOffsetMv => _relOffsetMv;

		internal int VminOffsetMv => _vminOffsetMv;

		internal int AltOffsetMv => _altOffsetMv;

		internal int OvOffsetMv => _ovOffsetMv;

		public MainWindow()
		{
			InitializeComponent();
			LoadStateAndSlots();

			/*
				Restore every offset the saved state carries, not just some of them.

				This used to read DemandCoreMv, DemandXbarMv and Nvvdd.RelUv and stop there, so a
				saved VMIN limit came back as zero. The chip's headline figures are read live from
				mVolt and were right, but the sliders under them were rebuilt from the offsets, and
				one of those was missing — the card said 900–1025 mV while its own lower slider sat
				at 625 and the formula read 625 + 0. Reloading a configuration that the program had
				just applied is exactly when this shows up.
			*/
			_voltOffsetMv = (int)_state.Voltage.DemandCoreMv;
			_xbarOffsetMv = (int)_state.Voltage.DemandXbarMv;
			_vminOffsetMv = (int)(_state.Voltage.Nvvdd.VminUv / 1000);
			_relOffsetMv = (int)(_state.Voltage.Nvvdd.RelUv / 1000);
			_altOffsetMv = (int)(_state.Voltage.Nvvdd.AltUv / 1000);
			_ovOffsetMv = (int)(_state.Voltage.Nvvdd.OvUv / 1000);

			// The title bar stays Windows', but is told to draw itself dark. Repainting the
			// caption by hand would mean reimplementing the buttons, snapping and resize borders;
			// this keeps all of that and changes only the colour. Hung off Loaded because the
			// window handle does not exist before then.
			base.Loaded += delegate
			{
				DarkTitleBar.Apply(this);
			};

			ApplyTooltips();
			base.Loaded += OnLoaded;
			base.ContentRendered += delegate
			{
				CenterOnWorkArea();
			};
			base.Closing += OnClosing;
		}

		/// <summary>
		/// The factory power wall, in watts.
		///
		/// Order matters here, and the reason is that the value cannot be re-read once it has
		/// been changed: anything read live afterwards is the number this program wrote.
		///
		///   1. A loaded, unmodified driver reports OemBaseline. Authoritative, and recorded.
		///   2. A recorded value beats any live reading, because the live reading may be ours.
		///   3. Only with nothing on record does the NVML fallback run, capturing whatever it
		///      finds. That is a first launch after a reboot, where the wall is still factory.
		///
		/// The previous version kept no record and went straight to the NVML reading, so after
		/// raising the ceiling to 200 W it reported 200 W as the factory wall.
		/// </summary>
		private int ReadPowerFloorW()
		{
			if (Driver.QueryStatus(out var status, out var _) && status.OemBaseline != 0)
			{
				int fromDriver = (int)(status.OemBaseline / 1000);
				if (fromDriver > 0 && _state.PowerFloorW != fromDriver)
				{
					_state.PowerFloorW = fromDriver;
					Store.Log("出厂功耗墙已记录: " + fromDriver + " W（驱动 OemBaseline）");
					SaveState();
				}
				return fromDriver;
			}

			if (_state.PowerFloorW > 0)
			{
				return _state.PowerFloorW;
			}

			EnvSample envSample = Telemetry.Sample();
			if (envSample.HasPowerLimit && envSample.EnforcedLimitW > 0.0)
			{
				int captured = (int)Math.Round(envSample.EnforcedLimitW);
				_state.PowerFloorW = captured;
				Store.Log("出厂功耗墙已记录: " + captured + " W（NVML，首次运行）");
				SaveState();
				return captured;
			}
			return 350;
		}

		private void ApplyTooltips()
		{
			string text = -25 + " … +" + 50 + " mV";
			Tip(PwFloorLabel, "");
			Tip(PwMinus, "目标上限 −" + 25 + " W。\n只改这个数字，点“应用功耗”才会真正下发。\n下限是出厂功耗墙——再往下没有意义，那是这张卡的设计功率。");
			Tip(PwPlus, "目标上限 +" + 25 + " W。\n驱动按 5 W 量化，所以步进取 25 W 这种整数，避免写出奇怪的值。\n上限是内核模块编译时写死的值，无法通过界面突破。");
			Tip(PwTargetNow, "你要申请的功耗上限。\n范围从出厂功耗墙到内核上限，已标在两侧；初值是当前生效上限，所以没调整时显示的就是现状。\n申请值 ≠ 生效值：驱动可能因为机型策略、温度或供电压到更低，真实值看左边的“当前上限”。");
			Tip(PwLimitNow, "此刻实际执行的上限。\n空闲与烤机时都会读同一个策略值，所以它不随负载变化。");
			Tip(PwNow, "GPU 当前实际功耗。空闲时只有几十瓦，烤机时才会接近上限。\n判断解锁是否真的生效，看这个值在负载下能不能超过出厂上限。");
			Tip(PwTemp, "核心温度 / 温度墙。\n余量 = 温度墙 − 核心温度，是判断提高功耗上限后能否吃满的依据。");
			Tip(VoltNow, "核心电压实时读数（µV 精度，每秒刷新）。\n来源是 NVAPI 的未公开接口 ClientVoltRailsGetStatus，已在本地验证过。");
			Tip(VoltApplied, "当前生效的核心轨偏移，也就是已经写进驱动里的值。\n与右侧「核心轨偏移」不同：那个是待下发的，这个是最新生效的。\n两者不一致说明有改动还没点“应用”。");
			Tip(VoltLimitsNow, "当前**实际生效**的电压上下限，从 mVolt+ 回读，不是待下发的值。\n下限 = 出厂下限 + VMIN 偏移；上限 = min(REL, ALT/OP, OV) 的评估结果。\n拖动滑块不会改变这里 —— 只有点“应用”并回读成功后才更新。\n注意这是电压策略值，不是实测电压；实测值看左边的“核心电压”。");
			Tip(VoltOffsetLabel, "待下发的核心电压需求偏移，范围 " + text + "。\n点“应用”后由 mVolt+ 写入，随后本程序回读确认。");
			Tip(ClkCore, "核心频率偏移，单位 MHz。\n合法范围标在输入框两侧，超出会被拒绝，不会下发给驱动。");
			Tip(ClkMem, "显存频率偏移，单位 MHz。\n与核心偏移独立；显存超频过高通常表现为画面异常而不是死机。");
			Tip(ClkNow, "核心频率实时读数（NVML）。\n空闲时会大幅降频，这是正常的省电行为，不是被限制。");
			Tip(MemNow, "显存时钟，与 nvidia-smi、MSI Center 同口径（本例 9001 MHz）。\nGDDR7 每个时钟传输两次数据，所以等效传输速率约为该值的 2 倍（约 18 Gbps）。\n本程序直接显示驱动的原始读数，不做换算——这样与其它工具对得上。");
		}

		private static void Tip(FrameworkElement element, string text)
		{
			if (element != null && !string.IsNullOrEmpty(text))
			{
				element.ToolTip = new ToolTip
				{
					Content = text,
					MaxWidth = 380.0
				};
			}
		}

		private void RefreshPrerequisites()
		{
			PreHost.Children.Clear();
			foreach (UnlockCheck item in UnlockDiagnostics.Gather())
			{
				string name = (item.Ok ? "Accent" : (item.Blocking ? "Danger" : "Warn"));
				string text = (item.Ok ? "✓ " : (item.Blocking ? "✕ " : "! ")) + item.Name + "：" + item.Detail;
				bool flag = !string.IsNullOrEmpty(item.ActionId);
				UnlockCheck captured = item;
				Border border = new Border
				{
					CornerRadius = new CornerRadius(4.0),
					Padding = new Thickness(8.0, 3.0, 8.0, 3.0),
					Margin = new Thickness(0.0, 0.0, 8.0, 0.0)
				};
				border.SetResourceReference(Border.BorderBrushProperty, name);
				border.SetResourceReference(Border.BackgroundProperty, flag ? "FieldBg" : "CardBg");
				border.BorderThickness = new Thickness(1.0);
				TextBlock textBlock = new TextBlock
				{
					FontSize = 11.0,
					Text = text
				};
				textBlock.SetResourceReference(TextBlock.ForegroundProperty, name);
				border.Child = textBlock;
				string text2 = PrerequisiteExplanation(item);
				if (flag)
				{
					text2 += "\n\n点击可切换（需要管理员权限，改动后需重启生效）。";
				}
				border.ToolTip = new ToolTip
				{
					Content = text2,
					MaxWidth = 380.0
				};
				if (flag)
				{
					border.Cursor = Cursors.Hand;
					border.MouseLeftButtonUp += delegate
					{
						OnPrerequisiteClick(captured);
					};
				}
				PreHost.Children.Add(border);
			}
		}

		private void OnPrerequisiteClick(UnlockCheck c)
		{
			/*
				The rest are toggles, so the direction depends on what is there now and the
				confirmation has to say which way it is going. A single pair of verbs derived from
				Ok does not work, because the chips do not all mean the same thing by Ok: for most
				of them it means the setting is already where the driver needs it and the click
				turns it off, but for the CI policies it means the files are gone and the click
				puts them back.
			*/
			string action = string.IsNullOrEmpty(c.ClickAction) ? c.Name : c.ClickAction;
			string caution = string.IsNullOrEmpty(c.ActionId)
				? ""
				: "\r\n\r\n注意：这些改动是为了让本程序的自签名内核驱动能够加载，会降低系统对恶意内核代码的防护。" +
				  "再点一次这一项可以改回去。";

			if (Confirm("要" + action + "吗？\r\n\r\n" + c.Detail + "\r\n\r\n这项改动需要重启才会生效。" + caution))
			{
				if (UnlockDiagnostics.Toggle(c.ActionId, out var message))
				{
					Store.Log("前提项已切换: " + action);
					Info(message);
				}
				else
				{
					Store.Log("前提项切换失败: " + action + " — " + message);
					Warn(message);
				}
				RefreshPrerequisites();
			}
		}

		/// <summary>
		/// The chip's tooltip.
		///
		/// ManualHint is preferred when present. The switch that used to be here keyed off the
		/// check's name, and the checks have been through several revisions — the names it knew
		/// about (测试签名, 内存完整性, 测试证书, 驱动签名) no longer exist, so every current chip
		/// fell through to the bare detail line and the tooltips said nothing about what the item
		/// was for or what to do about it.
		/// </summary>
		private static string PrerequisiteExplanation(UnlockCheck c)
		{
			return string.IsNullOrEmpty(c.ManualHint) ? ("当前：" + c.Detail) : c.ManualHint;
		}
		private void OnLoaded(object sender, RoutedEventArgs e)
		{
			//IL_0120: Unknown result type (might be due to invalid IL or missing references)
			//IL_0125: Unknown result type (might be due to invalid IL or missing references)
			//IL_013e: Expected O, but got Unknown
			Store.Log("NV显卡功耗软解 1.9.0 启动");
			CenterOnWorkArea();
			_gpuName = DetectGpu();
			_profile = Driver.DetectProfile(_gpuName);
			if (_state.Profile == 0)
			{
				_state.Profile = _profile;
			}
			SubtitleText.Text = _gpuName;
			Store.Log("GPU: " + _gpuName + " profile=" + _profile);
			_powerFloorW = ReadPowerFloorW();
			if (Driver.QueryStatus(out var status, out var _) && status.UpperBoundary != 0)
			{
				_targetW = (int)(status.UpperBoundary / 1000);
			}
			else
			{
				EnvSample envSample = Telemetry.Sample();
				_targetW = ((envSample.HasPowerLimit && envSample.EnforcedLimitW > 0.0) ? ((int)Math.Round(envSample.EnforcedLimitW)) : _powerFloorW);
			}
			if (_targetW < _powerFloorW)
			{
				_targetW = _powerFloorW;
			}
			FillCardNotes();
			RefreshServiceButton();

			/*
				Refresh first, then write the saved state into the controls.

				The other order set the sliders while _baseMinMv and _baseMaxMv were still their
				field-initialiser defaults, because those are derived inside RefreshVoltagePanel
				from what mVolt reports. PushStateToControls builds each slider from baseline plus
				offset, so running it first would place them against a guessed baseline and nothing
				later corrects them — the one-second timer deliberately leaves the pending sliders
				alone.
			*/
			RefreshAll(logIt: true);
			PushStateToControls();
			_refreshTimer = new DispatcherTimer
			{
				Interval = TimeSpan.FromSeconds(1.0)
			};
			_refreshTimer.Tick += delegate
			{
				RefreshDriverStatus(logIt: false);
				RefreshVoltagePanel();
				RefreshReadout();
			};
			_refreshTimer.Start();
		}

		private void FillCardNotes()
		{
			Driver.ProfileRange(_profile, _state.CeilingMw, out var loMw, out var hiMw);
			Tip(PwTargetNow, string.Format(CultureInfo.InvariantCulture,
				"你要申请的功耗上限。可以直接输入数字，也可以用左右两个按钮以 {0} W 为步进调整。\n" +
				"本机型可申请范围：{1}–{2} W。输入超出范围会被收到边界上，不是 5 的整数倍会就近取整。\n" +
				"输入后按回车或点到别处即生效（这只改申请值，还要点「应用」才会真正下发）。\n" +
				"申请值 ≠ 生效值：驱动可能因为机型策略、温度或供电压到更低，真实值看左边的「当前上限」。",
				25, loMw / 1000, hiMw / 1000));
			PwMaxLabel.Text = hiMw / 1000 + " W";
			Tip(PwFloorLabel, _powerFloorW + " W —— 这是从机器读取的出厂功耗墙，也就是这张卡的默认功率。\n内核模块的 OemBaseline 优先，读不到时用 NVML 的生效上限。\n它不是固定数字：不同机型/不同 VBIOS 会不一样。");
			Tip(PwFloorLabel, _powerFloorW + "（W）—— 出厂功耗墙，也就是这张卡的默认功率。\n再往下调没有意义：下限就到这里，减号会停住。\n（内核模块允许的最小申请值是 " + loMw / 1000 + " W，但那张卡的实际功耗墙是 " + _powerFloorW + " W。）");
			Tip(PwMaxLabel, string.Format(CultureInfo.InvariantCulture, "本机型策略窗口的上限：{0} W。\n高于此值申请会被内核模块拒绝。\n这是内核模块编译时写死的上限，无法通过界面突破。", hiMw / 1000));
			PwFloorLabel.Text = _powerFloorW + " W";
			PwFloorRef.Text = _powerFloorW + " W";
			Tip(PwFloorRef, _powerFloorW + " W —— 本行可调范围的下端，也就是这张卡的默认功率。\n点减号不会低于这里：再往下没有意义，那是这张卡的设计功率。");
			TuningState tuningState = Tuning.Query();
			if (!tuningState.CoreOk && !tuningState.MemoryOk && !tuningState.XbarOk)
			{
				// No writable ranges were reported, so the fields stay empty and greyed.
				return;
			}
			// The range now lives at the ends of each row, the way the power card states its floor
			// and ceiling, rather than in a sentence at the top of the card.
			ClkCoreMinLabel.Text = tuningState.CoreMin.ToString(CultureInfo.InvariantCulture);
			ClkCoreMaxLabel.Text = "+" + tuningState.CoreMax.ToString(CultureInfo.InvariantCulture);
			ClkMemMinLabel.Text = tuningState.MemoryMin.ToString(CultureInfo.InvariantCulture);
			ClkMemMaxLabel.Text = "+" + tuningState.MemoryMax.ToString(CultureInfo.InvariantCulture);
			Tip(ClkCore, string.Format(CultureInfo.InvariantCulture,
				"核心频率偏移，可写范围 {0}…+{1} MHz，步进 {2} MHz。\n超出范围会被拒绝，不会下发给驱动。",
				tuningState.CoreMin, tuningState.CoreMax, CLK_STEP_MHZ));
			Tip(ClkMem, string.Format(CultureInfo.InvariantCulture,
				"显存频率偏移，可写范围 {0}…+{1} MHz，步进 {2} MHz。\n超出范围会被拒绝，不会下发给驱动。",
				tuningState.MemoryMin, tuningState.MemoryMax, CLK_STEP_MHZ));
		}

		private void OnClosing(object sender, CancelEventArgs e)
		{
			if (_state.StartMinimized)
			{
				e.Cancel = true;
				base.WindowState = WindowState.Minimized;
				return;
			}
			if (_refreshTimer != null)
			{
				_refreshTimer.Stop();
			}

			/*
				The helper stays loaded when the window closes. That is what the reference does, and
				it is what makes the rest of the design work.

				The helper only loads against a factory baseline — it reads the ceiling at load time
				and treats it as the OEM value — so a session that unloads it cannot load it again
				once the ceiling has moved. Reloading then means a display device restart to put the
				ceiling back, which is a few seconds of black screen every time the window is closed
				and reopened. Inspecting the reference binary settles it: it has no unload-on-exit
				path at all. Its only teardown is a user-pressed "Restart Nvpwr driver", and that is
				the one that carries the "restoring OEM before unload" gate.

				The anti-cheat answer is DSE, not the driver, and that part is already tighter here
				than in the reference: this program disables DSE only for the instant the driver
				loads and restores it in a finally block, rather than holding it off for the whole
				session. Nothing is written to BCD and test signing is never enabled — the reference
				requires TESTSIGNING ON, which is far more visible than anything left here.

				Anyone who wants the kernel clean as well uses 标准模式, which restores the factory
				settings and removes the service, and boots without EfiGuard.
			*/
			if (Driver.IsOpenable())
			{
				Store.Log("内核驱动保持加载（功耗值在 NVIDIA 驱动内，退出不影响；DSE 已恢复）");
			}

			Store.Log("NvpwrControl 退出");
		}

		private void CenterOnWorkArea()
		{
			//IL_000a: Unknown result type (might be due to invalid IL or missing references)
			//IL_000f: Unknown result type (might be due to invalid IL or missing references)
			//IL_003c: Unknown result type (might be due to invalid IL or missing references)
			//IL_0041: Unknown result type (might be due to invalid IL or missing references)
			Rect workArea = SystemParameters.WorkArea;
			base.Left = Math.Max(0.0, (workArea.Width - base.Width) / 2.0);
			workArea = SystemParameters.WorkArea;
			base.Top = Math.Max(0.0, (workArea.Height - base.Height) / 2.0);
			TryGetClientSize(out var width, out var height);
			Store.Log(string.Format(CultureInfo.InvariantCulture, "窗口 {0:0}x{1:0}，客户区 {2:0}x{3:0}（设计 1280x720）", base.Width, base.Height, width, height));
		}

		private bool TryGetClientSize(out double width, out double height)
		{
			width = 0.0;
			height = 0.0;
			try
			{
				IntPtr handle = new WindowInteropHelper(this).Handle;
				if (handle == IntPtr.Zero)
				{
					return false;
				}
				if (!GetClientRect(handle, out var rect))
				{
					return false;
				}
				width = rect.Right - rect.Left;
				height = rect.Bottom - rect.Top;
				return width > 0.0 && height > 0.0;
			}
			catch
			{
				return false;
			}
		}

		[DllImport("user32.dll")]
		private static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

		private static string DetectGpu()
		{
			try
			{
				EnvSample envSample = Telemetry.Sample();
				if (!string.IsNullOrEmpty(envSample.GpuName))
				{
					return envSample.GpuName;
				}
			}
			catch
			{
			}
			return "未识别 NVIDIA 显卡";
		}

		private void LoadStateAndSlots()
		{
			_state = Store.LoadState(out var error);
			if (!string.IsNullOrEmpty(error))
			{
				Store.Log(error);
			}
			_slots = Store.LoadSlots(out error);
			if (!string.IsNullOrEmpty(error))
			{
				Store.Log(error);
			}
		}

		private void SaveState()
		{
			if (!Store.SaveState(_state, out var error))
			{
				Store.Log(error);
			}
		}

		private void SaveSlots()
		{
			if (!Store.SaveSlots(_slots, out var error))
			{
				Store.Log(error);
			}
		}

		private void RefreshAll(bool logIt)
		{
			RefreshDriverStatus(logIt);
			RefreshVoltagePanel();
			RefreshClocksPanel();
			RefreshReadout();
			RefreshPrerequisites();
		}

		private void RefreshDriverStatus(bool logIt)
		{
			if (!Driver.QueryStatus(out var status, out var error))
			{
				ActionHint.Text = error;
				ActionHint.SetResourceReference(TextBlock.ForegroundProperty, "Danger");
				if (logIt)
				{
					Store.Log("驱动状态不可用: " + error);
				}
				SetLimitFromTelemetry("出厂（驱动未加载）");
				return;
			}
			uint num = ((status.OemBaseline != 0) ? status.OemBaseline : status.UpperBoundary);
			if (status.UpperBoundary != 0)
			{
				PwLimitNow.Text = Watts(status.UpperBoundary);
				Tip(PwLimitNow, "驱动此刻实际执行的上限，来自内核模块回读。\n出厂基线 " + Watts(num) + "。" + ((status.UpperBoundary != num) ? "\n当前值高于基线，说明本程序已提升过上限。" : "\n与出厂基线一致，说明还没提升过。"));
			}
			else
			{
				SetLimitFromTelemetry("驱动未上报");
			}
			string text = Driver.StateText(status.State);
			switch (status.State)
			{
			case 2u:
			case 9u:
				ActionHint.SetResourceReference(TextBlock.ForegroundProperty, "TextDim");
				ActionHint.Text = "就绪 · " + text;
				break;
			case 3u:
				ActionHint.SetResourceReference(TextBlock.ForegroundProperty, "Danger");
				ActionHint.Text = text;
				break;
			default:
				ActionHint.SetResourceReference(TextBlock.ForegroundProperty, "Warn");
				ActionHint.Text = text;
				break;
			}
			if (status.ActiveProfile != 0)
			{
				_profile = status.ActiveProfile;
			}
			if (logIt)
			{
				Store.Log(string.Format(CultureInfo.InvariantCulture, "状态: state={0} oem={1} current={2} upper={3}", status.State, num, status.CurrentEffective, status.UpperBoundary));
			}
		}

		private void SetLimitFromTelemetry(string reason)
		{
			EnvSample envSample = Telemetry.Sample();
			if (envSample.HasPowerLimit)
			{
				PwLimitNow.Text = Fmt(envSample.EnforcedLimitW, " W");
				Tip(PwLimitNow, "实际生效的上限，来自 NVML（" + reason + "）。\n本程序的内核模块未加载，无法回读它自己的策略对象，所以这里用的是 NVIDIA 驱动对外报告的值。\n它与 nvidia-smi 的 enforced.power.limit 是同一个数。");
			}
			else
			{
				PwLimitNow.Text = "—";
				Tip(PwLimitNow, "无法读取生效上限：内核模块未加载，NVML 也没有返回该值。");
			}
		}

		private void RefreshClocksPanel()
		{
			TuningState tuningState = Tuning.Query();
			TextBox[] array = new TextBox[3] { ClkCore, ClkMem, ClkXbar };
			TextBlock[] array2 = new TextBlock[3] { ClkCoreMinLabel, ClkMemMinLabel, ClkXbarMinLabel };
			TextBlock[] array3 = new TextBlock[3] { ClkCoreMaxLabel, ClkMemMaxLabel, ClkXbarMaxLabel };
			bool[] array4 = new bool[3] { tuningState.CoreOk, tuningState.MemoryOk, tuningState.XbarOk };
			long[] array5 = new long[3] { tuningState.CoreMhz, tuningState.MemoryMhz, tuningState.XbarMhz };
			long[] array6 = new long[3] { tuningState.CoreMin, tuningState.MemoryMin, tuningState.XbarMin };
			long[] array7 = new long[3] { tuningState.CoreMax, tuningState.MemoryMax, tuningState.XbarMax };
			for (int i = 0; i < 3; i++)
			{
				array[i].IsEnabled = array4[i];
				if (!array[i].IsFocused)
				{
					array[i].Text = array5[i].ToString(CultureInfo.InvariantCulture);
				}
				array2[i].Text = (array4[i] ? array6[i].ToString(CultureInfo.InvariantCulture) : "—");
				array3[i].Text = (array4[i] ? ("+" + array7[i].ToString(CultureInfo.InvariantCulture)) : "—");
				array2[i].SetResourceReference(TextBlock.ForegroundProperty, array4[i] ? "Accent" : "TextDim");
				array3[i].SetResourceReference(TextBlock.ForegroundProperty, array4[i] ? "Danger" : "TextDim");
			}
			ClkXbarRow.Visibility = ((!tuningState.XbarOk) ? Visibility.Collapsed : Visibility.Visible);
		}

		private void RefreshVoltagePanel()
		{
			_mvoltPath = MVolt.Find(_state.MvoltPath);
			_mvoltReady = !string.IsNullOrEmpty(_mvoltPath) && File.Exists(_mvoltPath);
			VoltSlider.IsEnabled = _mvoltReady;
			XbarSlider.IsEnabled = _mvoltReady;
			OvSlider.IsEnabled = _mvoltReady;
			MinVoltSlider.IsEnabled = _mvoltReady;
			MaxVoltSlider.IsEnabled = _mvoltReady;
			MVoltSnapshot mVoltSnapshot = MVolt.QueryStatus(_state.MvoltPath);
			if (!mVoltSnapshot.Ok || mVoltSnapshot.NvvddLimitMaxMv <= 0)
			{
				return;
			}
			/*
				The factory limits: recorded once, then never overwritten.

				Deriving them from the live readings — reported limit minus the offset on that rail
				— only holds while OV is untouched, because an active OV offset moves the reported
				maximum without appearing in the REL value. That derivation is what turned a
				request for 1025 into REL +5 and landed the rail on 1030.

				Re-capturing whenever the offsets read as clean was no better. It races with this
				app's own writes: right after an apply the tool reports the new maximum while the
				offset field has not caught up, so the capture recorded 1080 as the factory value
				and every later request was computed against it. The log shows it happening twice
				in a row.

				So the stored value wins. It is only written when there is nothing on record yet and
				the rail is genuinely clean, which on a first run it is; otherwise the derivation is
				used as a provisional figure without being saved, and the next tick retries.
			*/
			if (_state.Voltage.BaselineMaxMv > 0)
			{
				_baseMinMv = (int)_state.Voltage.BaselineMinMv;
				_baseMaxMv = (int)_state.Voltage.BaselineMaxMv;
			}
			else
			{
				_baseMinMv = (int)(mVoltSnapshot.NvvddLimitMinMv - mVoltSnapshot.Nvvdd.VminUv / 1000);
				_baseMaxMv = (int)(mVoltSnapshot.NvvddLimitMaxMv - mVoltSnapshot.Nvvdd.RelUv / 1000);
				if (mVoltSnapshot.Nvvdd.IsZero)
				{
					_state.Voltage.BaselineMinMv = _baseMinMv;
					_state.Voltage.BaselineMaxMv = _baseMaxMv;
					Store.Log("电压基线已记录: " + _baseMinMv + "–" + _baseMaxMv + " mV");
					SaveState();
				}
			}
			_appliedMinMv = (int)mVoltSnapshot.NvvddLimitMinMv;
			_appliedMaxMv = (int)mVoltSnapshot.NvvddLimitMaxMv;
			if (mVoltSnapshot.NvvddMinUv > 0 && mVoltSnapshot.NvvddMaxUv > 0)
			{
				_absMinMv = (int)(mVoltSnapshot.NvvddMinUv / 1000);
				_absMaxMv = (int)(mVoltSnapshot.NvvddMaxUv / 1000);
				if (MinVoltSlider != null)
				{
					// Only the floor and the tick step come from the device range. The minimum's
					// upper bound is not the device maximum — it is whatever ceiling is in force,
					// which is narrower, so ClampMinSlider sets it from the current limits below.
					MinVoltSlider.Minimum = _absMinMv;
					MinVoltSlider.TickFrequency = ((mVoltSnapshot.NvvddStepUv > 0) ? ((double)mVoltSnapshot.NvvddStepUv / 1000.0) : 5.0);
					ClampMinSlider();
				}
			}

			// Note what is deliberately NOT done here: the pending sliders are not written back.
			//
			// This runs on the one-second timer, and pushing _relOffsetMv / _ovOffsetMv into the
			// controls made the refresh fight the user. Choosing 1000 mV routes the request to OV
			// and leaves REL at zero; the next tick then set the maximum slider back to
			// 1025 (= baseline + 0), which fired its handler, took the "at or above baseline"
			// branch and cleared the OV offset again. The edit vanished one second after it was
			// made.
			//
			// The sliders are the source of truth for pending edits; only an explicit load (slot,
			// undo, reset) writes into them, and that goes through PushStateToControls.
		}

		private void RefreshReadout()
		{
			EnvSample envSample = Telemetry.Sample();
			PwNow.Text = (envSample.HasPowerDraw ? Watts((uint)Math.Round(envSample.PowerDrawW * 1000.0)) : "—");
			ClkNow.Text = (envSample.HasCoreClock ? Fmt(envSample.CoreClockMhz, " MHz") : "—");
			UtilNow.Text = (envSample.HasUtilization ? Fmt(envSample.UtilizationPct, " %") : "—");
			MemNow.Text = (envSample.HasMemoryClock ? Fmt(envSample.MemoryClockMhz, " MHz") : "—");
			PwTemp.Text = ((envSample.HasTemp && envSample.HasSpeedThreshold) ? (Fmt(envSample.TempC, "") + "/" + Fmt(envSample.SpeedThresholdC, " °C")) : (envSample.HasTemp ? Fmt(envSample.TempC, " °C") : "—"));
			VoltLimitsNow.Text = _appliedMinMv + "–" + _appliedMaxMv + " mV";
			long demandCoreMv = _state.Voltage.DemandCoreMv;
			VoltApplied.Text = ((demandCoreMv > 0) ? "+" : "") + demandCoreMv + " mV";
			VoltApplied.SetResourceReference(TextBlock.ForegroundProperty, (demandCoreMv == 0L) ? "TextMain" : "Info");
			long microVolts;
			string error;
			bool flag = Tuning.TryReadVoltageUv(out microVolts, out error);
			VoltNow.Text = (flag ? (((double)microVolts / 1000000.0).ToString("0.000", CultureInfo.InvariantCulture) + " V") : "—");
			VoltNow.SetResourceReference(TextBlock.ForegroundProperty, flag ? "Info" : "TextDim");
			RefreshDriverVersion(envSample);
		}

				/// <summary>
		/// Puts the driver build in the header, in red when it is not the supported one.
		///
		/// Red rather than a footnote: on an unsupported build nothing else in this window can
		/// be trusted to mean what it says, so it is the first thing worth seeing.
		/// </summary>
		private void RefreshDriverVersion(EnvSample env)
		{
			string v = env.DriverVersion;
			if (string.IsNullOrEmpty(v))
			{
				DriverText.Text = "—";
				DriverText.SetResourceReference(TextBlock.ForegroundProperty, "TextDim");
				DriverText.ToolTip = "读不到驱动版本。";
				return;
			}

			bool supported = v.Equals(SUPPORTED_DRIVER, StringComparison.OrdinalIgnoreCase);
			DriverText.Text = v;
			DriverText.SetResourceReference(TextBlock.ForegroundProperty, supported ? "Accent" : "Danger");
			DriverText.ToolTip = supported
				? ("本程序只适配 " + SUPPORTED_DRIVER + " 版本的 NVIDIA 驱动。\r\n当前版本匹配。")
				: ("当前驱动 " + v + " 不是本程序适配的 " + SUPPORTED_DRIVER + "。\r\n\r\n" +
				   "内核驱动按 " + SUPPORTED_DRIVER + " 的内部布局读写功耗策略，版本不符时偏移会错位，" +
				   "因此不会尝试下发。请安装 " + SUPPORTED_DRIVER + " 版本的驱动。");
		}

private static string Fmt(double v, string unit)
		{
			return v.ToString("0.#", CultureInfo.InvariantCulture) + unit;
		}

		private static string Watts(uint mw)
		{
			if (mw == 0 || mw == uint.MaxValue)
			{
				return "0.0 W";
			}
			return ((double)mw / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) + " W";
		}

		// ------------------------------------------------------------------ power target

		/// <summary>
		/// The window the kernel will accept, in whole watts.
		///
		/// Two lower bounds exist and the larger wins. The profile window is what the kernel
		/// module accepts for this GPU; the factory wall is what the card was built for, and
		/// asking below it is pointless rather than dangerous. On the reference machine both
		/// are 175 W, so they agree, but they come from different places and either could move.
		/// </summary>
		private void PowerWindow(out int lo, out int hi)
		{
			uint ceiling = ((_state.CeilingMw != 0) ? _state.CeilingMw : 350000u);
			uint loMw, hiMw;
			Driver.ProfileRange(_profile, ceiling, out loMw, out hiMw);
			lo = (int)(loMw / 1000);
			hi = (int)(hiMw / 1000);
			if (lo < _powerFloorW) lo = _powerFloorW;
			if (hi < lo) hi = lo;
		}

		/// <summary>Writes the pending target into the field, formatted the way it is entered.</summary>
		private void ShowPowerTarget()
		{
			if (PwTargetNow != null)
			{
				PwTargetNow.Text = _targetW.ToString(CultureInfo.InvariantCulture);
			}
		}

		/// <summary>
		/// Reads what was typed, brings it inside the window, and snaps it to the step the
		/// kernel quantises to.
		///
		/// Clamped rather than refused. Someone who types 400 meant "as high as it goes", and
		/// someone who types 213 meant 215 — the kernel only accepts multiples of 5, which is
		/// why the buttons step by 25 and why a typed number is rounded rather than rejected
		/// with a complaint. Whatever is adjusted is said out loud underneath, because a field
		/// that silently rewrites what you typed is worse than one that refuses it.
		/// </summary>
		private void CommitPowerTarget()
		{
			if (PwTargetNow == null) return;

			int typed;
			if (!int.TryParse(PwTargetNow.Text.Trim(), NumberStyles.Integer,
					  CultureInfo.InvariantCulture, out typed))
			{
				ShowPowerTarget();
				return;
			}

			int lo, hi;
			PowerWindow(out lo, out hi);

			int wanted = Math.Max(lo, Math.Min(hi, typed));
			int snapped = (int)(Math.Round(wanted / 5.0, MidpointRounding.AwayFromZero) * 5.0);
			if (snapped < lo) snapped = lo;
			if (snapped > hi) snapped = hi;

			_targetW = snapped;
			ShowPowerTarget();

			if (PwHint != null)
			{
				if (typed != snapped)
				{
					PwHint.Text = "已调整为 " + snapped + " W（可调范围 " + lo + "–" + hi +
								  " W，步进 5 W）。";
				}
				else
				{
					PwHint.Text = "";
				}
			}
		}

		private void OnPowerTargetCommit(object sender, RoutedEventArgs e)
		{
			CommitPowerTarget();
		}

		private void OnPowerTargetKey(object sender, KeyEventArgs e)
		{
			if (e.Key == Key.Enter)
			{
				CommitPowerTarget();
				e.Handled = true;
			}
			else if (e.Key == Key.Escape)
			{
				// Put back what was there and leave the field, so a half-typed value cannot
				// be left sitting in a control that looks like an applied setting.
				ShowPowerTarget();
				PwHint.Text = "";
				Keyboard.ClearFocus();
				PwPlus.Focus();
				e.Handled = true;
			}
		}

		private void OnPowerMinus(object sender, RoutedEventArgs e)
		{
			int lo, hi;
			PowerWindow(out lo, out hi);
			_targetW = Math.Max(lo, _targetW - POWER_STEP_W);
			PwHint.Text = "";
			ShowPowerTarget();
		}

		private void OnPowerPlus(object sender, RoutedEventArgs e)
		{
			int lo, hi;
			PowerWindow(out lo, out hi);
			_targetW = Math.Min(hi, _targetW + POWER_STEP_W);
			PwHint.Text = "";
			ShowPowerTarget();
		}

		private void OnPowerReset(object sender, RoutedEventArgs e)
		{
			_targetW = _powerFloorW;
			PwHint.Text = "";
			ShowPowerTarget();
		}

		private void OnApplyPower(object sender, RoutedEventArgs e)
		{
			ApplyPowerRequest(_targetW);
		}

		private bool ApplyPowerRequest(int watts)
		{
			if (watts != 0 && watts % 5 != 0)
			{
				Warn("目标功耗必须是 5 W 的整数倍。");
				return false;
			}
			uint num = (uint)(watts * 1000);
			uint num2 = ((_state.CeilingMw != 0) ? _state.CeilingMw : 350000u);
			if (num != 0)
			{
				Driver.ProfileRange(_profile, num2, out var loMw, out var hiMw);
				if (hiMw != 0 && (num < loMw || num > hiMw))
				{
					Warn(string.Format(CultureInfo.InvariantCulture, "目标功耗超出本机型范围：{0}–{1} W。", loMw / 1000, hiMw / 1000));
					return false;
				}
			}
			if (num > 225000 && !Confirm("确认应用 " + watts + " W？\r\n\r\n这超过本类机型已验证的上限。\r\n\r\n提高功耗上限会增加 GPU、供电模块、显存与整机散热的电气和热负荷，电源适配器可能无法长期支撑。\r\n\r\n注意：如果本次开机中已经改过功耗上限，应用前会先重置一次显卡设备，屏幕会黑几秒、程序会短暂无响应 —— 这是必须的步骤，不是故障。"))
			{
				return false;
			}
			string error;

			/*
				Bring the kernel helper up if it is not already running.

				This is what closes signature enforcement, starts the service and waits for the
				device object — several seconds, and a brief window without DSE. It only runs when
				the device is closed, so once the helper is up every further apply in this session
				is a plain IOCTL.

				It may also restart the display device first. The helper identifies the factory
				baseline by recognising a stock-looking limit and refuses to work once the limit
				has been changed, so a rail that is no longer at the factory value has to be put
				back before a fresh helper can be loaded at all.
			*/
			/*
				The refresh tick has to be off for everything below it.

				EnsureLoaded may restart the display device — that is how the helper gets back to a
				factory baseline once the ceiling has been changed — and the tick reads the GPU
				through NVML and NVAPI every second. Those are native calls into a driver that is
				in the middle of being torn down, and they do not fail politely: measured, this
				crashed the process with an access violation inside coreclr at 0x1d4089 while the
				card was being removed. The screen going black for a few seconds is the restart
				itself and is expected; the process dying was not.

				Stopped here rather than inside EnsureLoaded so it covers the applies that follow
				too, and resumed in a finally so no early return can leave the window frozen.
			*/
			SuspendRefresh();
			try
			{
				bool deviceRestarted = false;

				/*
					A loaded helper is not necessarily a usable one.

					The helper reads the ceiling at load time and takes it for the OEM value. Loaded
					while the ceiling had already moved, the baseline it records is wrong — measured
					both ways: it came back as 0 in one run and as the moved value, 250000, in
					another. Either way the arithmetic is done against a baseline that is not the
					factory one, and a set for anything below it is refused with error 87. That is
					not hypothetical: the background service does this whenever it comes up while
					the rail is off factory, which is what a stopped-and-restarted service looks
					like.

					So the test is not "is the driver open" but "does the baseline it reports match
					the factory value on record". That record lives in state.ini and is preferred
					over anything read live, precisely so it survives this. A mismatch means the
					helper is dropped here and the load below goes through EnsureLoaded, which puts
					the rail back to that factory value first.
				*/
				if (Driver.IsOpenable())
				{
					Driver.Status loaded;
					string probe;
					if (Driver.QueryStatus(out loaded, out probe))
					{
						bool noBaseline = loaded.OemBaseline == 0;
						bool wrongBaseline = _state.PowerFloorW > 0 &&
											 loaded.OemBaseline / 1000 != (uint)_state.PowerFloorW;
						if (noBaseline || wrongBaseline)
						{
							Store.Log("驱动已加载但基线不可信（报告 " + loaded.OemBaseline / 1000 +
									  " W，应为 " + _state.PowerFloorW + " W），卸载后重新走加载流程");
							UnlockChain.Unload();
						}
					}
				}

				if (!Driver.IsOpenable() &&
					!UnlockChain.EnsureLoaded(_powerFloorW, out deviceRestarted, out error))
				{
					Warn(error);
					return false;
				}

				if (num == 0 || watts <= _powerFloorW)
				{
					Store.SetRestorePoint("恢复出厂前", _state);
					if (!Driver.Restore(out error))
					{
						Warn(error);
						return false;
					}
					_state.PowerEnabled = false;
					_state.PowerMw = 0u;
				}
				else
				{
					Store.SetRestorePoint("应用功耗前", _state);
					uint profile = ((_state.Profile != 0) ? _state.Profile : _profile);
					if (!Driver.SetPower(num, num2, profile, out error))
					{
						Warn(error);
						return false;
					}
					_state.PowerEnabled = true;
					_state.PowerMw = num;
				}
				if (_state.Profile == 0)
				{
					_state.Profile = _profile;
				}
				SaveState();
				Store.Log("应用功耗: " + watts + " W (上限 " + num2 / 1000 + " W)");
				RefreshAll(logIt: false);
				RefreshDriverStatus(logIt: false);

				/*
					Put the other two settings back after a restart.

					Reloading nvlddmkm discards the voltage and clock offsets along with the power
					ceiling, so a power change that needed a restart used to leave the card at its
					factory voltage while the interface still showed what had been applied — the
					two halves of the card disagreeing, with nothing on screen to explain it.

					Only after a restart: otherwise those values are still in the driver and
					re-sending them is pointless work.
				*/
				if (deviceRestarted)
				{
					ReapplyVoltageAndClocks();
				}
				return true;
			}
			finally
			{
				ResumeRefresh();
			}
		}

		/// <summary>
		/// Re-sends the saved voltage and clock settings, for use after the display device has
		/// been restarted underneath them. Failures are logged rather than shown: the power
		/// change the user asked for has already succeeded, and a modal on top of that would
		/// read as "the whole thing failed".
		/// </summary>
		private void ReapplyVoltageAndClocks()
		{
			if (_state.Voltage.Enabled && !_state.Voltage.Nvvdd.IsZero && _mvoltReady)
			{
				if (MVolt.Apply(_state.MvoltPath, _state.Voltage, out var verr, out var _))
				{
					Store.Log("显卡设备重置后已重新下发电压");
				}
				else
				{
					Store.Log("⚠ 显卡设备重置后重新下发电压失败: " + verr);
				}
			}

			if (_state.Clock.Enabled)
			{
				TuningState live = Tuning.Query();
				if (live.CoreOk || live.MemoryOk || live.XbarOk)
				{
					string cerr;
					if (Tuning.Apply(_state.Clock, live, out cerr))
					{
						Store.Log("显卡设备重置后已重新下发频率");
					}
					else
					{
						Store.Log("⚠ 显卡设备重置后重新下发频率失败: " + cerr);
					}
				}
			}

			RefreshAll(logIt: false);
		}

		/// <summary>
		/// Stops the one-second refresh for the duration of a driver operation.
		///
		/// The tick samples the GPU through NVML and NVAPI. While a display device restart is in
		/// progress those become calls into hardware that is going away, and the failure mode is
		/// a crash rather than an error code — see the note in ApplyPowerRequest.
		/// </summary>
		private void SuspendRefresh()
		{
			if (_refreshTimer != null)
			{
				_refreshTimer.Stop();
			}
		}

		private void ResumeRefresh()
		{
			if (_refreshTimer != null)
			{
				_refreshTimer.Start();
			}
		}

		/// <summary>
		/// Writes the pending offsets into the controls.
		///
		/// Only for an explicit load — a slot, an undo, a reset. This is deliberately not called
		/// from the one-second refresh: pushing pending values back into the sliders there made the
		/// refresh fight the user, because with the ceiling routed to OV the REL value is zero and
		/// the tick would set the maximum slider to 1025, whose handler then cleared the OV offset.
		/// An edit survived about a second.
		///
		/// The maximum is written from the ceiling rather than from REL, since the ceiling is what
		/// that control represents; REL alone would show the baseline whenever OV is doing the work.
		/// </summary>
		private void UpdateVoltOffsetLabel()
		{
			SetSlider(VoltSlider, VoltOffsetLabel, _voltOffsetMv);
			SetSlider(XbarSlider, XbarOffsetLabel, _xbarOffsetMv);
			SetSliderQuiet(OvSlider, _ovOffsetMv);

			SetSlider(MinVoltSlider, MinVoltLabel, _baseMinMv + _vminOffsetMv);
			int ceiling = Math.Min(_baseMaxMv + _relOffsetMv, OV_BASE_MV + _ovOffsetMv);
			SetSliderQuiet(MaxVoltSlider, ceiling);
			if (MaxVoltLabel != null)
			{
				MaxVoltLabel.Text = ceiling + " mV";
			}

			UpdateOvLabels();
			ClampMinSlider();
			UpdateOffsetSummary();
		}

		private static void SetSlider(Slider s, TextBlock label, int value)
		{
			if (s != null && Math.Abs(s.Value - (double)value) > 0.001)
			{
				s.Value = value;
			}
			if (label != null)
			{
				label.Text = value + " mV";
			}
		}

		private static string Signed(int mv)
		{
			return ((mv > 0) ? "+" : "") + mv + " mV";
		}

		private void OnVoltSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
		{
			_voltOffsetMv = (int)Math.Round(e.NewValue);
			if (VoltOffsetLabel != null)
			{
				VoltOffsetLabel.Text = _voltOffsetMv + " mV";
			}
		}

		private void OnXbarSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
		{
			_xbarOffsetMv = (int)Math.Round(e.NewValue);
			if (XbarOffsetLabel != null)
			{
				XbarOffsetLabel.Text = _xbarOffsetMv + " mV";
			}
		}

		private void OnMinVoltSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
		{
			if (MinVoltLabel != null)
			{
				int num = (int)Math.Round(e.NewValue);
				_vminOffsetMv = num - _baseMinMv;
				MinVoltLabel.Text = num + " mV";
				UpdateOffsetSummary();
			}
		}

		/// <summary>
		/// Set while one handler writes into the other's slider. WPF raises ValueChanged for a
		/// programmatic assignment just as it does for a drag, so without this the two controls
		/// would call each other and overwrite the values they had just been given.
		/// </summary>
		private bool _syncingVoltControls;

		/// <summary>
		/// The maximum ceiling, as a voltage the user picks, routed to whichever offset can
		/// produce it.
		///
		/// REL accepts only 0…+125 on this rail (1025…1150 mV) and the tool refuses negative
		/// values with "within the application bounds", so REL alone cannot lower the ceiling —
		/// which is why setting 1000 mV first came back as "mVolt+ 未保留 NVDD 偏移（请求
		/// rel=-25 mV，回读 rel=0 mV）". OV can lower it, and does: its ceiling sits at 1200 mV
		/// and pulling it down caps the effective maximum once it passes below the REL limit. The
		/// companion tool reaches its 445-1150 range the same way.
		///
		/// ALT/OP is always left at zero. This rail driver reports no operating limit for it and
		/// the tool rejects any non-zero request outright with "The driver does not report an
		/// operating limit (ALT/OP) for this rail." Copying the REL value into it, which this
		/// method used to do, made every apply fail.
		///
		/// This is the master control for the ceiling. The OV slider below is linked to it rather
		/// than independent: both describe the same ceiling, so whichever is moved has to move the
		/// other, or they would sit there disagreeing about a value they both claim to set.
		/// </summary>
		private void OnMaxVoltSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
		{
			if (MaxVoltLabel == null || _syncingVoltControls)
			{
				return;
			}
			int num = (int)Math.Round(e.NewValue);
			_altOffsetMv = 0;
			if (num >= _baseMaxMv)
			{
				_relOffsetMv = num - _baseMaxMv;
				_ovOffsetMv = 0;
			}
			else
			{
				_relOffsetMv = 0;
				_ovOffsetMv = num - OV_BASE_MV;
			}
			MaxVoltLabel.Text = num + " mV";
			SetSliderQuiet(OvSlider, _ovOffsetMv);
			UpdateOvLabels();
			UpdateOffsetSummary();
			ClampMinSlider();
		}

		/// <summary>
		/// Moves a slider without re-entering its handler.
		///
		/// Assigning Value raises ValueChanged exactly as a drag does, so the linked controls have
		/// to be written through here or each would undo the other.
		/// </summary>
		private void SetSliderQuiet(Slider slider, int value)
		{
			if (slider == null || Math.Abs(slider.Value - (double)value) < 0.001)
			{
				return;
			}
			_syncingVoltControls = true;
			try
			{
				slider.Value = value;
			}
			finally
			{
				_syncingVoltControls = false;
			}
		}

		/// <summary>
		/// Holds the minimum at or below the ceiling in force.
		///
		/// The tool rejects a minimum above the upper limits with "The minimum-voltage offset
		/// conflicts with the current upper voltage limits", so the slider is kept inside what can
		/// be written instead of being allowed to build a request that is certain to fail.
		/// </summary>
		private void ClampMinSlider()
		{
			if (MinVoltSlider == null)
			{
				return;
			}
			int num = Math.Min(_baseMaxMv + _relOffsetMv, OV_BASE_MV + _ovOffsetMv);
			MinVoltSlider.Maximum = Math.Max(ABS_MIN_MV, num);
			if (MinVoltSlider.Value > MinVoltSlider.Maximum)
			{
				MinVoltSlider.Value = MinVoltSlider.Maximum;
			}
		}

		private void UpdateOvLabels()
		{
			if (OvLabel != null)
			{
				OvLabel.Text = "待下发 " + Signed(_ovOffsetMv) + "  →  OV 限值 " + (OV_BASE_MV + _ovOffsetMv) + " mV";
			}
		}

		private void UpdateOffsetSummary()
		{
			if (MinFormulaLabel != null)
			{
				MinFormulaLabel.Text = "下限  " + _baseMinMv + " + " + _vminOffsetMv + " = " + (_baseMinMv + _vminOffsetMv) + " mV";
				int num = _baseMaxMv + _relOffsetMv;
				int num2 = OV_BASE_MV + _ovOffsetMv;
				bool flag = num2 < num;
				MaxFormulaLabel.Text = "上限  " + _baseMaxMv + " + " + _relOffsetMv + " = " + num + " mV";
				if (OvParenthetical != null)
				{
					OvParenthetical.Text = (flag ? ("(OV " + num2 + " 将封顶 → 上限 " + num2 + " mV)") : ("(OV " + num2 + ")"));
					OvParenthetical.SetResourceReference(TextBlock.ForegroundProperty, flag ? "Warn" : "TextDim");
				}
				MaxFormulaLabel.SetResourceReference(TextBlock.ForegroundProperty, flag ? "Warn" : "TextMain");
			}
		}

		/// <summary>
		/// The OV slider, linked to the maximum above rather than independent of it.
		///
		/// Both controls describe the same ceiling, so dragging this one has to move that one too.
		/// Leaving them independent was the confusing part: moving 最高电压 to 1000 wrote OV -200
		/// and the OV slider followed, but dragging OV afterwards left 最高电压 claiming a ceiling
		/// that was no longer in force. REL is not touched here — the ceiling is simply whatever
		/// the tighter of the two limits yields.
		/// </summary>
		private void OnOvSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
		{
			if (OvLabel == null || _syncingVoltControls)
			{
				return;
			}
			_ovOffsetMv = (int)Math.Round(e.NewValue);

			int num = Math.Min(_baseMaxMv + _relOffsetMv, OV_BASE_MV + _ovOffsetMv);
			SetSliderQuiet(MaxVoltSlider, num);
			MaxVoltLabel.Text = num + " mV";

			UpdateOvLabels();
			UpdateOffsetSummary();
			ClampMinSlider();
		}

		private void OnVoltReset(object sender, RoutedEventArgs e)
		{
			_voltOffsetMv = 0;
			_xbarOffsetMv = 0;
			_vminOffsetMv = 0;
			_relOffsetMv = 0;
			_altOffsetMv = 0;
			_ovOffsetMv = 0;
			UpdateVoltOffsetLabel();
		}

		private void OnApplyVoltage(object sender, RoutedEventArgs e)
		{
			if (!_mvoltReady)
			{
				Warn("未找到 mVolt+。本程序不能直接写电压，需要 mVolt+ 执行写入。\r\n请把它放到本程序同目录，或在“设置”里指定路径。");
				return;
			}
			if (_voltOffsetMv == 0 && _xbarOffsetMv == 0 && _vminOffsetMv == 0 && _relOffsetMv == 0 && _altOffsetMv == 0 && _ovOffsetMv == 0)
			{
				if (Confirm("把三组电压偏移全部归零？"))
				{
					if (!MVolt.Reset(_state.MvoltPath, out var error))
					{
						Warn(error);
						return;
					}
					_state.Voltage = new VoltageTuning();
					SaveState();
					Store.Log("电压偏移已全部归零");
					RefreshVoltagePanel();
					RefreshReadout();
				}
				return;
			}
			/*
				Every line is short and every line break is an explicit CRLF.

				The Win32 message box does not wrap long text, so the single-sentence warning that
				used to end this message stretched the dialog past the edge of the window and its
				buttons were cut off at 200% scaling. The offsets were also joined with a bare LF,
				which the control does not treat as a line break — it needs CRLF.
			*/
			string text =
				"VMIN   限值偏移 " + Signed(_vminOffsetMv) + "\r\n" +
				"REL    限值偏移 " + Signed(_relOffsetMv) + "\r\n" +
				"ALT/OP 限值偏移 " + Signed(_altOffsetMv) + "\r\n" +
				"OV     限值偏移 " + Signed(_ovOffsetMv) + "\r\n" +
				"核心   需求偏移 " + Signed(_voltOffsetMv) + "\r\n" +
				"XBAR   需求偏移 " + Signed(_xbarOffsetMv);
			string warning =
				"电压修改可能导致显卡不稳定、驱动重置，\r\n" +
				"极端情况会损坏供电轨。\r\n" +
				"驱动接受了数值不等于稳定。";
			if (!Confirm("确认下发以下电压偏移？\r\n\r\n" + text + "\r\n\r\n" + warning))
			{
				return;
			}
			Store.SetRestorePoint("应用电压前", _state);
			DesiredState desiredState = _state.Clone();
			desiredState.Voltage.DemandCoreMv = _voltOffsetMv;
			desiredState.Voltage.DemandXbarMv = _xbarOffsetMv;
			desiredState.Voltage.Nvvdd = new RailOffsets
			{
				VminUv = (long)_vminOffsetMv * 1000L,
				RelUv = (long)_relOffsetMv * 1000L,
				AltUv = (long)_altOffsetMv * 1000L,
				OvUv = (long)_ovOffsetMv * 1000L
			};
			desiredState.Voltage.Enabled = true;
			if (!MVolt.Apply(_state.MvoltPath, desiredState.Voltage, out var error2, out var rounded))
			{
				Warn(error2);
				return;
			}
			if (rounded)
			{
				Store.Log("电压偏移已取整到整毫伏");
			}
			_state.Voltage = desiredState.Voltage;
			SaveState();
			Store.Log("电压已下发: core=" + _voltOffsetMv + " xbar=" + _xbarOffsetMv + " relLimit=" + _relOffsetMv + " mV");
			ArmPending();
			RefreshVoltagePanel();
			RefreshReadout();
		}

		private void OnOpenMVoltFull(object sender, RoutedEventArgs e)
		{
			string error;
			if (!_mvoltReady)
			{
				Warn("未找到 mVolt+。请把它放到本程序同目录，或在“设置”里指定路径。");
			}
			else if (!MVolt.Launch(_state.MvoltPath, out error))
			{
				Warn(error);
			}
			else
			{
				Store.Log("已启动 mVolt+（细节调节）");
			}
		}

		private void CollectClocks(DesiredState into)
		{
			TextBox[] array = new TextBox[3] { ClkCore, ClkMem, ClkXbar };
			long[] array2 = new long[3];
			for (int i = 0; i < 3; i++)
			{
				if (!long.TryParse(array[i].Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var result))
				{
					result = 0L;
				}
				array2[i] = result;
			}
			into.Clock.CoreOffsetMhz = array2[0];
			into.Clock.MemoryOffsetMhz = array2[1];
			into.Clock.XbarOffsetMhz = array2[2];
			into.Clock.Enabled = !into.Clock.IsZero;
		}

		private bool ValidateClocks(out string error)
		{
			error = null;
			TuningState tuningState = Tuning.Query();
			TextBox[] array = new TextBox[3] { ClkCore, ClkMem, ClkXbar };
			bool[] array2 = new bool[3] { tuningState.CoreOk, tuningState.MemoryOk, tuningState.XbarOk };
			long[] array3 = new long[3] { tuningState.CoreMin, tuningState.MemoryMin, tuningState.XbarMin };
			long[] array4 = new long[3] { tuningState.CoreMax, tuningState.MemoryMax, tuningState.XbarMax };
			string[] array5 = new string[3] { "核心", "显存", "XBAR" };
			for (int i = 0; i < 3; i++)
			{
				if (array2[i])
				{
					if (!long.TryParse(array[i].Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var result))
					{
						error = array5[i] + "偏移必须是整数 MHz。";
						return false;
					}
					if (result < array3[i] || result > array4[i])
					{
						error = string.Format(CultureInfo.InvariantCulture, "{0}偏移超出本机范围：允许 {1} … {2} MHz，你填的是 {3}。", array5[i], array3[i], array4[i], result);
						return false;
					}
				}
			}
			return true;
		}

		private void PushStateToControls()
		{
			ShowPowerTarget();
			UpdateVoltOffsetLabel();
			ClkCore.Text = _state.Clock.CoreOffsetMhz.ToString(CultureInfo.InvariantCulture);
			ClkMem.Text = _state.Clock.MemoryOffsetMhz.ToString(CultureInfo.InvariantCulture);
			ClkXbar.Text = _state.Clock.XbarOffsetMhz.ToString(CultureInfo.InvariantCulture);
		}

		// ------------------------------------------------------------------ clock steppers

		private void OnClkCoreMinus(object sender, RoutedEventArgs e) { StepClock(ClkCore, -CLK_STEP_MHZ); }
		private void OnClkCorePlus(object sender, RoutedEventArgs e) { StepClock(ClkCore, CLK_STEP_MHZ); }
		private void OnClkMemMinus(object sender, RoutedEventArgs e) { StepClock(ClkMem, -CLK_STEP_MHZ); }
		private void OnClkMemPlus(object sender, RoutedEventArgs e) { StepClock(ClkMem, CLK_STEP_MHZ); }
		private void OnClkXbarMinus(object sender, RoutedEventArgs e) { StepClock(ClkXbar, -CLK_STEP_MHZ); }
		private void OnClkXbarPlus(object sender, RoutedEventArgs e) { StepClock(ClkXbar, CLK_STEP_MHZ); }

		/// <summary>
		/// Nudges a clock offset field by one step and keeps it inside the writable range.
		///
		/// Clamped rather than allowed past the ends: a value outside the range is refused at
		/// apply time, so letting the stepper walk into it would only produce a rejection the user
		/// then has to undo. The ranges are read once here rather than cached, because they come
		/// from the driver and can differ per domain.
		/// </summary>
		private void StepClock(TextBox box, int delta)
		{
			if (box == null || !box.IsEnabled)
			{
				return;
			}
			TuningState ranges = Tuning.Query();
			int low, high;
			if (box == ClkCore)
			{
				low = (int)ranges.CoreMin;
				high = (int)ranges.CoreMax;
			}
			else if (box == ClkMem)
			{
				low = (int)ranges.MemoryMin;
				high = (int)ranges.MemoryMax;
			}
			else
			{
				// No range is reported for XBAR — the driver does not expose one — and the row is
				// hidden whenever that is so. The field is disabled in that case and the guard at
				// the top of this method has already returned; these bounds are only here so the
				// value cannot run away if that ever changes.
				low = (int)ranges.CoreMin;
				high = (int)ranges.CoreMax;
			}

			int value;
			if (!int.TryParse(box.Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value))
			{
				value = 0;
			}
			value += delta;
			if (value < low)
			{
				value = low;
			}
			if (value > high)
			{
				value = high;
			}
			box.Text = value.ToString(CultureInfo.InvariantCulture);
			box.CaretIndex = box.Text.Length;
		}

		/// <summary>
		/// Rejects anything but digits and a leading minus as it is typed.
		///
		/// The offsets are whole megahertz; letting a decimal point in would only produce a value
		/// that fails validation later, with nothing to say why.
		/// </summary>
		private void OnClockIntegerOnly(object sender, TextCompositionEventArgs e)
		{
			foreach (char c in e.Text)
			{
				if (!char.IsDigit(c) && c != '-')
				{
					e.Handled = true;
					return;
				}
			}
		}

		private void OnApplyClocks(object sender, RoutedEventArgs e)
		{
			DesiredState desiredState = _state.Clone();
			CollectClocks(desiredState);
			TuningState tuningState = Tuning.Query();
			string error;
			if (!tuningState.CoreOk && !tuningState.MemoryOk && !tuningState.XbarOk)
			{
				Warn("Pstates20 在此显卡/驱动上未开放可写的频率偏移范围。");
			}
			else if (!ValidateClocks(out error))
			{
				Warn(error);
			}
			else if (Confirm("确认应用频率偏移？\r\n\r\n频率过高可能导致画面异常、驱动重置或系统不稳定。"))
			{
				Store.SetRestorePoint("应用频率前", _state);
				if (!Tuning.Apply(desiredState.Clock, tuningState, out var error2))
				{
					Warn(error2);
					return;
				}
				_state.Clock = desiredState.Clock;
				SaveState();
				Store.Log("频率偏移已应用");
				ArmPending();
			}
		}

		private void OnResetClocks(object sender, RoutedEventArgs e)
		{
			Tuning.Reset(out var _);
			_state.Clock = new ClockTuning();
			ClkCore.Text = "0";
			ClkMem.Text = "0";
			ClkXbar.Text = "0";
			SaveState();
			RefreshClocksPanel();
			Store.Log("频率偏移已重置");
		}

		private void OnApplyAll(object sender, RoutedEventArgs e)
		{
			List<string> list = new List<string>();
			if (!ValidateClocks(out var error))
			{
				Warn(error);
				return;
			}
			DesiredState desiredState = _state.Clone();
			desiredState.PowerEnabled = _targetW > _powerFloorW;
			desiredState.PowerMw = (uint)(_targetW * 1000);
			if (desiredState.Profile == 0)
			{
				desiredState.Profile = _profile;
			}
			CollectClocks(desiredState);
			WritePendingVoltage(desiredState);
			if (desiredState.PowerMw > 225000 && !Confirm("确认一次性应用功耗、电压与频率？\r\n\r\n目标功耗 " + _targetW + " W 超过本类机型已验证的上限。\r\n\r\n会增加 GPU、供电与整机散热的负荷。\r\n\r\n注意：如果本次开机中已经改过功耗上限，应用前会先重置一次显卡设备，屏幕会黑几秒、程序会短暂无响应 —— 这是必须的步骤，不是故障。"))
			{
				return;
			}
			Store.SetRestorePoint("一键应用前", _state);
			Store.Log(string.Format(CultureInfo.InvariantCulture, "一键应用: power={0}W voltage={1}mV clocks={2}", _targetW, _voltOffsetMv, desiredState.Clock.Enabled ? 1 : 0));

			/*
				This used to call Driver.SetPower straight away, which meant the button worked only
				if something else had already brought the helper up. Raising the ceiling needs the
				helper loaded, and loading it needs the factory baseline back — the same chain the
				power card's 应用 runs. It goes through EnsureLoaded now.

				Which also means it can restart the display device, so the refresh tick is stopped
				first for the reason spelled out in ApplyPowerRequest.
			*/
			SuspendRefresh();
			try
			{
			string error0;
			if (desiredState.PowerEnabled && desiredState.PowerMw != 0 &&
				!Driver.IsOpenable() && !UnlockChain.EnsureLoaded(_powerFloorW, out error0))
			{
				list.Add("驱动：" + error0);
			}

			string error2;
			if (desiredState.PowerEnabled && desiredState.PowerMw != 0)
			{
				uint ceilingMw = ((_state.CeilingMw != 0) ? _state.CeilingMw : 350000u);
				if (!Driver.SetPower(desiredState.PowerMw, ceilingMw, desiredState.Profile, out error2))
				{
					list.Add("功耗：" + error2);
				}
			}
			else if (!Driver.Restore(out error2))
			{
				list.Add("恢复出厂：" + error2);
			}
			if (desiredState.Voltage.Enabled && _mvoltReady)
			{
				if (!MVolt.Apply(_state.MvoltPath, desiredState.Voltage, out var error3, out var _))
				{
					list.Add("电压：" + error3);
				}
			}
			else if (!desiredState.Voltage.Enabled && _mvoltReady && _state.Voltage.DemandCoreMv != 0L)
			{
				MVolt.Reset(_state.MvoltPath, out var _);
			}
			else if (desiredState.Voltage.Enabled && !_mvoltReady)
			{
				list.Add("电压：未找到 mVolt+");
			}
			if (desiredState.Clock.Enabled)
			{
				TuningState tuningState = Tuning.Query();
				if (tuningState.CoreOk || tuningState.MemoryOk || tuningState.XbarOk)
				{
					if (!Tuning.Apply(desiredState.Clock, tuningState, out var error5))
					{
						list.Add("频率：" + error5);
					}
				}
				else
				{
					list.Add("频率：Pstates20 未开放可写的偏移范围");
				}
			}
			_state = desiredState;
			SaveState();
			RefreshAll(logIt: false);
			}
			finally
			{
				ResumeRefresh();
			}
			if (list.Count == 0)
			{
				Store.Log("一键应用: 全部环节成功");
				if (desiredState.Clock.Enabled || desiredState.Voltage.Enabled)
				{
					ArmPending();
				}
				return;
			}
			Store.Log("一键应用部分失败: " + string.Join(" / ", list.ToArray()));
			string text = "部分生效：已成功下发的环节保持生效，以下环节失败——\r\n\r\n";
			foreach (string item in list)
			{
				text = text + "· " + item + "\r\n";
			}
			Warn(text);
			if (desiredState.Clock.Enabled || desiredState.Voltage.Enabled)
			{
				ArmPending();
			}
		}

		private void OnUndo(object sender, RoutedEventArgs e)
		{
			string label;
			DesiredState restorePoint = Store.GetRestorePoint(out label);
			if (restorePoint == null)
			{
				Info("没有可撤销的修改。");
				return;
			}
			string error;
			if (restorePoint.PowerEnabled && restorePoint.PowerMw != 0)
			{
				Driver.SetPower(restorePoint.PowerMw, restorePoint.CeilingMw, (restorePoint.Profile != 0) ? restorePoint.Profile : _profile, out error);
			}
			else
			{
				Driver.Restore(out error);
			}
			if (restorePoint.Voltage.Enabled && !restorePoint.Voltage.IsZero && _mvoltReady)
			{
				MVolt.Apply(_state.MvoltPath, restorePoint.Voltage, out var _, out var _);
			}
			else if (_mvoltReady)
			{
				MVolt.Reset(_state.MvoltPath, out var _);
			}
			if (restorePoint.Clock.Enabled)
			{
				TuningState live = Tuning.Query();
				Tuning.Apply(restorePoint.Clock, live, out var _);
			}
			_state = restorePoint;
			_targetW = (int)(restorePoint.PowerMw / 1000);
			_voltOffsetMv = (int)restorePoint.Voltage.DemandCoreMv;
			_xbarOffsetMv = (int)restorePoint.Voltage.DemandXbarMv;
			_vminOffsetMv = (int)(restorePoint.Voltage.Nvvdd.VminUv / 1000);
			_relOffsetMv = (int)(restorePoint.Voltage.Nvvdd.RelUv / 1000);
			_altOffsetMv = (int)(restorePoint.Voltage.Nvvdd.AltUv / 1000);
			_ovOffsetMv = (int)(restorePoint.Voltage.Nvvdd.OvUv / 1000);
			SaveState();
			PushStateToControls();
			Store.Log("已撤销到: " + label);
			DisarmPending();
			RefreshAll(logIt: false);
		}

		internal void RestoreDefaults()
		{
			if (Confirm("一键恢复默认？\r\n\r\n功耗回到出厂上限，电压与频率偏移清零。"))
			{
				Store.SetRestorePoint("恢复默认前", _state);
				Driver.Restore(out var error);
				if (_mvoltReady)
				{
					MVolt.Reset(_state.MvoltPath, out var _);
				}
				Tuning.Reset(out error);
				_state = DesiredState.Default();
				_state.Profile = _profile;

				/*
					Drop the recorded factory limits so they are re-read.

					The rail is clean at this point — every offset has just been cleared — so the
					reported limits ARE the factory ones again. Re-reading is also how a baseline
					that was recorded wrongly gets corrected; there is otherwise no way back, since
					the stored value deliberately wins over anything derived from live readings.
				*/
				_state.Voltage.BaselineMinMv = 0;
				_state.Voltage.BaselineMaxMv = 0;

				_targetW = 0;
				_voltOffsetMv = 0;
				SaveState();
				PushStateToControls();
				DisarmPending();
				Store.Log("已恢复默认");
				RefreshAll(logIt: false);
			}
		}

		private void OnRestoreDefaults(object sender, RoutedEventArgs e)
		{
			RestoreDefaults();
		}

		internal void ApplySlot(ConfigSlot slot, out bool ok, out List<string> failures)
		{
			failures = new List<string>();
			ok = true;
			DesiredState desiredState = slot.State.Clone();
			Store.SetRestorePoint("载入槽位前", _state);
			string error;
			if (desiredState.PowerEnabled && desiredState.PowerMw != 0)
			{
				if (!Driver.SetPower(desiredState.PowerMw, desiredState.CeilingMw, desiredState.Profile, out error))
				{
					ok = false;
					failures.Add("功耗：" + error);
				}
			}
			else if (!Driver.Restore(out error))
			{
				ok = false;
				failures.Add("恢复出厂：" + error);
			}
			if (desiredState.Voltage.Enabled && !desiredState.Voltage.IsZero)
			{
				if (_mvoltReady)
				{
					if (!MVolt.Apply(_state.MvoltPath, desiredState.Voltage, out var error2, out var _))
					{
						ok = false;
						failures.Add("电压：" + error2);
					}
				}
				else
				{
					ok = false;
					failures.Add("电压：未找到 mVolt+");
				}
			}
			if (desiredState.Clock.Enabled)
			{
				TuningState live = Tuning.Query();
				if (!Tuning.Apply(desiredState.Clock, live, out var error3))
				{
					ok = false;
					failures.Add("频率：" + error3);
				}
			}
			_state = desiredState;
			_targetW = (int)(desiredState.PowerMw / 1000);
			_voltOffsetMv = (int)desiredState.Voltage.DemandCoreMv;
			_xbarOffsetMv = (int)desiredState.Voltage.DemandXbarMv;
			_vminOffsetMv = (int)(desiredState.Voltage.Nvvdd.VminUv / 1000);
			_relOffsetMv = (int)(desiredState.Voltage.Nvvdd.RelUv / 1000);
			_altOffsetMv = (int)(desiredState.Voltage.Nvvdd.AltUv / 1000);
			_ovOffsetMv = (int)(desiredState.Voltage.Nvvdd.OvUv / 1000);
			SaveState();
			PushStateToControls();
			RefreshAll(logIt: false);
		}

		private void OnOpenSettings(object sender, RoutedEventArgs e)
		{
			if (_settingsDialog != null && _settingsDialog.IsVisible)
			{
				_settingsDialog.Activate();
				return;
			}
			_settingsDialog = new SettingsDialog(this, _state);
			_settingsDialog.ShowDialog();
			SaveState();
			RefreshAll(logIt: false);
		}

		private void OnToggleService(object sender, RoutedEventArgs e)
		{
			if (Service.Query() != "未安装")
			{
				if (!Confirm("卸载后台服务？\r\n\r\n卸载后开机不会自动重放功耗与频率设置，重启后需要手动再点一次“一键应用全部”。"))
				{
					return;
				}
				if (!Service.Remove(out var error))
				{
					Warn(error);
					return;
				}
				Store.Log("后台服务已卸载");
				Info("后台服务已卸载。");
			}
			else
			{
				if (!Service.Install(out var error2))
				{
					Warn(error2);
					return;
				}
				Store.Log("后台服务已安装");
				Info("后台服务已安装。开机后会自动重新下发功耗与电压。");
			}
			RefreshServiceButton();
			RefreshAll(logIt: false);
		}

		private void RefreshServiceButton()
		{
			string text = Service.Query();
			bool flag = text != "未安装";
			ServiceButton.Content = (flag ? "卸载服务" : "安装服务");
			ServiceButton.ToolTip = new ToolTip
			{
				Content = (flag ? ("后台服务已安装（" + text + "）。\n它在开机和从休眠恢复时重新下发功耗与频率，让设置在重启后继续生效。\n点击卸载。") : "安装后台服务。\nGPU 的功耗策略只存在于驱动内存，重启必然丢失；服务会在开机时自动重放你的设置。\n点击安装。"),
				MaxWidth = 380.0
			};
		}

		private void OnOpenSlots(object sender, RoutedEventArgs e)
		{
			if (_slotsDialog != null && _slotsDialog.IsVisible)
			{
				_slotsDialog.Activate();
				return;
			}
			_slotsDialog = new SlotsDialog(this, _slots);
			_slotsDialog.ShowDialog();
			SaveSlots();
			RefreshAll(logIt: false);
		}

		/// <summary>
		/// Keeps the restore point that was taken before the write, and logs it.
		///
		/// This used to raise a banner along the bottom offering 确认保留 / 立即回滚. That was
		/// dropped: every apply already goes through a confirmation dialog, so asking a second
		/// time immediately afterwards was just noise. The restore point itself stays — 撤销 in
		/// the bottom bar is still the way back, and it is the part that actually protects the
		/// machine.
		/// </summary>
		private void ArmPending()
		{
			Store.Log("已下发，恢复点已保留");
		}

		/// <summary>Retained so the call sites that ended a pending window stay readable.</summary>
		private void DisarmPending()
		{
		}

		internal RailOffsets PendingRailOffsets()
		{
			return new RailOffsets
			{
				VminUv = (long)_vminOffsetMv * 1000L,
				RelUv = (long)_relOffsetMv * 1000L,
				AltUv = (long)_altOffsetMv * 1000L,
				OvUv = (long)_ovOffsetMv * 1000L
			};
		}

		internal bool AnyVoltagePending()
		{
			if (_voltOffsetMv == 0 && _xbarOffsetMv == 0 && _vminOffsetMv == 0 && _relOffsetMv == 0 && _altOffsetMv == 0)
			{
				return _ovOffsetMv != 0;
			}
			return true;
		}

		internal void WritePendingVoltage(DesiredState into)
		{
			into.Voltage.DemandCoreMv = _voltOffsetMv;
			into.Voltage.DemandXbarMv = _xbarOffsetMv;
			into.Voltage.Nvvdd = PendingRailOffsets();
			into.Voltage.Enabled = AnyVoltagePending();
		}

		internal void Warn(string message)
		{
			ConfirmDialog.Ask(this, message, withCancel: false);
		}

		internal void Info(string message)
		{
			ConfirmDialog.Ask(this, message, withCancel: false);
		}

		internal bool Confirm(string message)
		{
			return ConfirmDialog.Ask(this, message, withCancel: true);
		}

	}
}