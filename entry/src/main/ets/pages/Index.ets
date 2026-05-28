import { common } from '@kit.AbilityKit';
import XComponentContext from '../interface/XComponentContext';
import { AudioCaptureService } from '../services/AudioCaptureService';
import { ImageSaveResult, ImageSaveService } from '../services/ImageSaveService';

class ModeOption {
  value: string;
  decoderValue: string;

  constructor(value: string, decoderValue: string) {
    this.value = value;
    this.decoderValue = decoderValue;
  }
}

@Entry
@Component
struct Index {
  private readonly decoderModes: Array<ModeOption> = [
    new ModeOption('Auto', 'AUTO'),
    new ModeOption('Robot36', 'Robot 36 Color'),
    new ModeOption('Robot72', 'Robot 72 Color'),
    new ModeOption('Martin1', 'Martin 1'),
    new ModeOption('Martin2', 'Martin 2'),
    new ModeOption('Scottie1', 'Scottie 1'),
    new ModeOption('Scottie2', 'Scottie 2'),
    new ModeOption('ScottieDX', 'Scottie DX'),
    new ModeOption('Wraase180', 'Wraase SC2-180'),
    new ModeOption('HFFax', 'HF Fax'),
    new ModeOption('PD50', 'PD 50'),
    new ModeOption('PD120', 'PD 120'),
    new ModeOption('PD180', 'PD 180'),
    new ModeOption('PD240', 'PD 240'),
    new ModeOption('PD290', 'PD 290'),
    new ModeOption('Raw', 'Raw Mode')
  ];
  @State nativeState: string = 'Awaiting XComponent load';
  @State debugLabel: string = 'Robot36 Harmony receiver';
  @State isCapturing: boolean = false;
  @State selectedMode: string = 'AUTO';
  @State autoSaveEnabled: boolean = true;
  @State showSettings: boolean = false;
  @State showDebugState: boolean = false;
  @State showModeSelector: boolean = false;
  @State showingAbout: boolean = false;
  @State statusMode: string = 'Idle';
  @State statusSignal: string = '0.0000';
  @State statusFrames: string = '0';
  @State statusSync: string = '0';
  @State statusError: string = '';
  @State statusSaved: string = '';
  @State viewportWidth: number = 1080;
  @State viewportHeight: number = 1920;
  private xComponentContext?: XComponentContext;
  private audioCaptureService?: AudioCaptureService;
  private pollTimer?: number;
  private lastImageCount: number = 0;
  private saveInFlight: boolean = false;
  private surfaceInitialized: boolean = false;

  private getHostAbilityContext(): common.UIAbilityContext {
    return this.getUIContext().getHostContext() as common.UIAbilityContext;
  }

  private isCompactLayout(): boolean {
    return this.viewportWidth < 420 || this.viewportHeight < 760;
  }

  private isTinyWindow(): boolean {
    return this.viewportWidth < 320 || this.viewportHeight < 560;
  }

  private getSurfaceHeight(): string {
    if (!this.isCompactLayout()) {
      return '58%';
    }
    if (this.isTinyWindow()) {
      return '60%';
    }
    return '64%';
  }

  private async openRepoLink(): Promise<void> {
    try {
      await this.getHostAbilityContext().startAbility({
        uri: 'https://github.com/WeiguangTWK/Robot36-OH',
      });
    } catch (_ignore) {
    }
  }

  private updateViewport(width: number, height: number): void {
    if (width > 0) {
      this.viewportWidth = width;
    }
    if (height > 0) {
      this.viewportHeight = height;
    }
  }

  private parseNativeState(raw: string): Map<string, string> {
    const values = new Map<string, string>();
    for (const token of raw.split(' | ')) {
      const separator: number = token.indexOf('=');
      if (separator > 0) {
        values.set(token.slice(0, separator), token.slice(separator + 1));
      }
    }
    return values;
  }

  private syncNativeState(): void {
    if (!this.xComponentContext) {
      return;
    }
    this.nativeState = this.xComponentContext.getNativeState();
    const parsed = this.parseNativeState(this.nativeState);
    this.statusMode = parsed.get('mode') ?? 'Idle';
    this.statusSignal = parsed.get('peak') ?? '0.0000';
    this.statusFrames = parsed.get('images') ?? '0';
    this.statusSync = parsed.get('sync') ?? '0';
    const imageCount: number = Number(this.statusFrames);
    const running = parsed.get('running') ?? 'no';
    const runtimeError = parsed.get('error') ?? '';
    if (runtimeError.length > 0) {
      this.statusError = runtimeError;
    }
    this.isCapturing = running === 'yes';
    if (!Number.isNaN(imageCount) && imageCount > this.lastImageCount) {
      this.lastImageCount = imageCount;
      void this.trySaveLatestImage(imageCount);
    }
  }

  private startPolling(): void {
    if (this.pollTimer !== undefined) {
      clearInterval(this.pollTimer);
    }
    this.pollTimer = setInterval(() => {
      this.syncNativeState();
    }, 250);
  }

  private applyMode(mode: string): void {
    this.selectedMode = mode;
    this.showModeSelector = false;
    if (this.audioCaptureService) {
      this.audioCaptureService.setDecoderMode(mode);
      this.syncNativeState();
    }
  }

  private getSelectedModeIndex(): number {
    for (let i = 0; i < this.decoderModes.length; i += 1) {
      if (this.decoderModes[i].decoderValue === this.selectedMode) {
        return i;
      }
    }
    return 0;
  }

  private getSelectedModeLabel(): string {
    if (this.selectedMode === 'AUTO') {
      const resolvedMode = this.statusMode.length > 0 ? this.statusMode : 'Robot 36 Color';
      return `${resolvedMode} (Auto)`;
    }
    return this.decoderModes[this.getSelectedModeIndex()].value;
  }

  private async startCapture(): Promise<void> {
    if (!this.audioCaptureService) {
      return;
    }
    try {
      await this.audioCaptureService.start();
      this.statusError = '';
      this.statusSaved = '';
      this.syncNativeState();
    } catch (err) {
      this.statusError = `capture start failed: ${JSON.stringify(err)}`;
    }
  }

  private async stopCapture(): Promise<void> {
    if (!this.audioCaptureService) {
      return;
    }
    try {
      await this.audioCaptureService.stop();
    } finally {
      this.syncNativeState();
    }
  }

  private async trySaveLatestImage(imageCount: number): Promise<void> {
    if (!this.xComponentContext || this.saveInFlight) {
      return;
    }
    if (!this.autoSaveEnabled) {
      this.statusSaved = `image ${imageCount} ready to save`;
      return;
    }
    await this.saveLatestImage(imageCount);
  }

  private resolveExportModeName(): string {
    if (this.selectedMode === 'AUTO') {
      if (this.statusMode.length > 0 && this.statusMode !== 'Idle' && this.statusMode !== 'Auto Mode') {
        return this.statusMode;
      }
      return 'Robot 36 Color';
    }
    return this.selectedMode;
  }

  private buildImageFileName(imageIndex: number): string {
    return ImageSaveService.buildSuggestedBmpName(new Date(), this.resolveExportModeName(), imageIndex);
  }

  private async saveLatestImage(imageIndex: number): Promise<void> {
    if (!this.xComponentContext || this.saveInFlight) {
      return;
    }
    const latestImage: ArrayBuffer | null = this.xComponentContext.exportLatestImageBmp();
    if (latestImage === null) {
      this.statusError = 'no completed image available';
      return;
    }

    this.saveInFlight = true;
    try {
      const fileName = this.buildImageFileName(imageIndex);
      const saveResult: ImageSaveResult = await ImageSaveService.saveBmp(this.getHostAbilityContext(), fileName, latestImage);
      this.handleSaveResult(saveResult);
    } catch (err) {
      this.statusError = `image save failed: ${JSON.stringify(err)}`;
      this.statusSaved = '';
    } finally {
      this.saveInFlight = false;
    }
  }

  private handleSaveResult(saveResult: ImageSaveResult): void {
    if (saveResult.status === 'saved') {
      this.statusSaved = `saved ${saveResult.path}`;
      this.statusError = '';
      return;
    }
    if (saveResult.status === 'cancelled') {
      this.statusSaved = saveResult.message;
      this.statusError = '';
      return;
    }
    this.statusSaved = '';
  }

  private async saveCurrentImage(): Promise<void> {
    const imageIndex = Math.max(this.lastImageCount, 1);
    await this.saveLatestImage(imageIndex);
  }

  onPageHide(): void {
    if (this.audioCaptureService) {
      void this.audioCaptureService.pauseForBackground().then(() => {
        this.syncNativeState();
      });
    }
  }

  onPageShow(): void {
    if (this.audioCaptureService) {
      void this.audioCaptureService.resumeFromBackground().then((resumed: boolean) => {
        if (resumed) {
          this.statusError = '';
        }
        this.syncNativeState();
      }).catch((err: Error) => {
        this.statusError = `resume failed: ${JSON.stringify(err)}`;
      });
    }
  }

  aboutToDisappear(): void {
    if (this.pollTimer !== undefined) {
      clearInterval(this.pollTimer);
      this.pollTimer = undefined;
    }
    if (this.audioCaptureService) {
      void this.audioCaptureService.release();
      this.audioCaptureService = undefined;
    }
  }

  @Builder
  private buildAboutPage(): void {
    Column({ space: 18 }) {
      Row({ space: 12 }) {
        Button($r('app.string.about_back'))
          .height(38)
          .padding({ left: 14, right: 14 })
          .borderRadius(12)
          .backgroundColor('#E9ECEF')
          .fontColor('#14213D')
          .onClick(() => {
            this.showingAbout = false;
          })

        Text($r('app.string.about_title'))
          .fontSize(24)
          .fontWeight(FontWeight.Bold)
          .fontColor('#14213D')
          .layoutWeight(1)
      }
      .alignItems(VerticalAlign.Center)

      Column({ space: 10 }) {
        Text($r('app.string.EntryAbility_label'))
          .fontSize(22)
          .fontWeight(FontWeight.Bold)
          .fontColor('#14213D')

        Text($r('app.string.about_summary'))
          .fontSize(15)
          .fontColor('#4F5D75')
          .lineHeight(22)
      }
      .alignItems(HorizontalAlign.Start)
      .padding(20)
      .backgroundColor('#FFFFFF')
      .borderRadius(24)

      Column({ space: 10 }) {
        Text($r('app.string.about_origin_title'))
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor('#14213D')

        Text($r('app.string.about_origin_body'))
          .fontSize(14)
          .fontColor('#4F5D75')
          .lineHeight(22)

        Text($r('app.string.about_repo_label'))
          .fontSize(14)
          .fontColor('#1D4ED8')
          .decoration({ type: TextDecorationType.Underline })
          .onClick(async () => {
            await this.openRepoLink();
          })
      }
      .alignItems(HorizontalAlign.Start)
      .padding(20)
      .backgroundColor('#FFFFFF')
      .borderRadius(24)
    }
    .padding(20)
    .width('100%')
    .height('100%')
    .alignItems(HorizontalAlign.Start)
    .backgroundColor('#E9EEF4')
    .zIndex(1)
  }

  @Builder
  private buildHomePage(): void {
    Scroll() {
      Column({ space: this.isTinyWindow() ? 10 : 18 }) {
        Row({ space: 12 }) {
          Text($r('app.string.stage0_title'))
            .fontSize(this.isTinyWindow() ? 22 : 28)
            .fontWeight(FontWeight.Bold)
            .fontColor('#20242B')
            .layoutWeight(1)

          Button($r('app.string.about_icon'))
            .width(24)
            .height(24)
            .borderRadius(12)
            .backgroundColor(Color.Transparent)
            .border({
              width: 1,
              color: '#20242B',
            })
            .fontColor('#20242B')
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .padding(0)
            .onClick(() => {
              this.showingAbout = true;
            })
        }
        .alignItems(VerticalAlign.Center)
        .width('100%')

        XComponent({ id: 'robot36NativeSurface', type: XComponentType.SURFACE, libraryname: 'entry' })
          .onLoad((xComponentContext) => {
            this.xComponentContext = xComponentContext as XComponentContext;
            this.xComponentContext.setDebugLabel(this.debugLabel);
            if (!this.surfaceInitialized) {
              this.surfaceInitialized = true;
              this.xComponentContext.drawTestFrame();
              this.audioCaptureService = new AudioCaptureService(this.getHostAbilityContext(), this.xComponentContext);
              this.startPolling();
              this.syncNativeState();
              void this.startCapture();
              void this.audioCaptureService.resumeFromBackground().then((resumed: boolean) => {
                if (resumed) {
                  this.statusError = '';
                }
                this.syncNativeState();
              }).catch((err: Error) => {
                this.statusError = `resume failed: ${JSON.stringify(err)}`;
              });
            }
          })
          .backgroundColor('#0E1116')
          .borderRadius(24)
          .width('100%')
          .height(this.getSurfaceHeight())
          .constraintSize({
            minHeight: this.isTinyWindow() ? 220 : 280,
            maxHeight: this.isTinyWindow() ? 360 : 520
          })

        Row({ space: 10 }) {
          Column({ space: 4 }) {
            Text($r('app.string.current_mode'))
              .fontSize(10)
              .fontColor('#86A3BF')
            if (this.showModeSelector) {
              Select(this.decoderModes)
                .selected(this.getSelectedModeIndex())
                .value(this.getSelectedModeLabel())
                .onSelect((index: number) => {
                  this.applyMode(this.decoderModes[index].decoderValue);
                })
            } else {
              Text(this.getSelectedModeLabel())
                .fontSize(this.isTinyWindow() ? 14 : 16)
                .fontWeight(FontWeight.Medium)
                .fontColor('#EAF3FF')
            }
          }
          .padding(this.isTinyWindow() ? 10 : 14)
          .layoutWeight(1)
          .backgroundColor('#15202B')
          .borderRadius(18)
          .onClick(() => {
            this.showModeSelector = !this.showModeSelector;
          })
        }
        .width('100%')

        Column({ space: this.isTinyWindow() ? 10 : 14 }) {
          Row({ space: 10 }) {
            Button($r('app.string.save_image'))
              .layoutWeight(1)
              .backgroundColor('#2C7BFF')
              .onClick(async () => this.saveCurrentImage())
            Button(this.showSettings ? $r('app.string.hide_settings') : $r('app.string.show_settings'))
              .layoutWeight(1)
              .backgroundColor('#203247')
              .onClick(() => {
                this.showSettings = !this.showSettings;
              })
          }

          if (this.statusError.length > 0) {
            Text(this.statusError)
              .fontSize(12)
              .fontColor('#FFB4A2')
          }

          if (this.statusSaved.length > 0) {
            Text(this.statusSaved)
              .fontSize(12)
              .fontColor('#8BD3AE')
          }

          if (this.showSettings) {
            Column({ space: 12 }) {
              Text($r('app.string.settings_title'))
                .fontSize(14)
                .fontWeight(FontWeight.Medium)
                .fontColor('#EAF3FF')

              Row({ space: 12 }) {
                Text($r('app.string.auto_save'))
                  .fontSize(13)
                  .fontColor('#C7D2DD')
                  .layoutWeight(1)
                Toggle({ type: ToggleType.Switch, isOn: this.autoSaveEnabled })
                  .onChange((value: boolean) => {
                    this.autoSaveEnabled = value;
                    this.statusSaved = value ? this.statusSaved : '';
                  })
              }

              Row({ space: 12 }) {
                Text($r('app.string.show_debug'))
                  .fontSize(13)
                  .fontColor('#C7D2DD')
                  .layoutWeight(1)
                Toggle({ type: ToggleType.Switch, isOn: this.showDebugState })
                  .onChange((value: boolean) => {
                    this.showDebugState = value;
                  })
              }

              Text($r('app.string.mode_tip'))
                .fontSize(12)
                .fontColor('#7F97AE')
            }
          }

          if (this.showDebugState && !this.isTinyWindow()) {
            Column({ space: 6 }) {
              Text($r('app.string.stage0_status_prefix'))
                .fontSize(12)
                .fontColor('#7F97AE')
              Text(this.nativeState)
                .fontSize(12)
                .fontColor('#C7D2DD')
                .maxLines(this.isCompactLayout() ? 8 : 12)
            }
            .alignItems(HorizontalAlign.Start)
          }
        }
        .padding(this.isTinyWindow() ? 12 : 18)
        .backgroundColor('#151A22')
        .borderRadius(22)
        .width('100%')
      }
      .onAreaChange((_oldValue: Area, newValue: Area) => {
        this.updateViewport(Number(newValue.width), Number(newValue.height));
      })
      .padding(this.isTinyWindow() ? 12 : 18)
      .width('100%')
    }
    .scrollBar(BarState.Off)
    .height('100%')
    .width('100%')
    .backgroundColor('#E9EEF4')
  }

  build() {
    Stack() {
      this.buildHomePage();
      if (this.showingAbout) {
        this.buildAboutPage();
      }
    }
    .width('100%')
    .height('100%')
  }
}
