import { abilityAccessCtrl, common } from '@kit.AbilityKit';
import { audio } from '@kit.AudioKit';
import XComponentContext from '../interface/XComponentContext';

export class AudioCaptureService {
  private static resumeAfterBackgroundFlag: boolean = false;
  private static lastSelectedMode: string = 'AUTO';
  private capturer: audio.AudioCapturer | null = null;
  private started: boolean = false;
  private currentMode: string = AudioCaptureService.lastSelectedMode;
  private readonly context: common.UIAbilityContext;
  private readonly xComponentContext: XComponentContext;
  private readonly sampleRate: number;

  constructor(context: common.UIAbilityContext, xComponentContext: XComponentContext, sampleRate: number = 44100) {
    this.context = context;
    this.xComponentContext = xComponentContext;
    this.sampleRate = sampleRate;
  }

  private async ensureMicrophonePermission(): Promise<string | null> {
    let atManager: abilityAccessCtrl.AtManager;
    try {
      atManager = abilityAccessCtrl.createAtManager();
    } catch (error) {
      return `Failed to create access manager: ${JSON.stringify(error)}`;
    }
    const tokenId: number = this.context.applicationInfo.accessTokenId;
    let grantStatus: abilityAccessCtrl.GrantStatus;
    try {
      grantStatus = atManager.checkAccessTokenSync(tokenId, 'ohos.permission.MICROPHONE');
    } catch (error) {
      return `Failed to check microphone permission: ${JSON.stringify(error)}`;
    }
    if (grantStatus === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) {
      return null;
    }

    try {
      const result = await atManager.requestPermissionsFromUser(this.context, ['ohos.permission.MICROPHONE']);
      if (result && result.authResults && result.authResults.length > 0 &&
        result.authResults[0] === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) {
        return null;
      }
    } catch (error) {
      return `Failed to request microphone permission: ${JSON.stringify(error)}`;
    }

    let recheck: abilityAccessCtrl.GrantStatus;
    try {
      recheck = atManager.checkAccessTokenSync(tokenId, 'ohos.permission.MICROPHONE');
    } catch (error) {
      return `Failed to recheck microphone permission: ${JSON.stringify(error)}`;
    }
    if (recheck === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) {
      return null;
    }

    try {
      await atManager.requestPermissionOnSetting(this.context, ['ohos.permission.MICROPHONE']);
    } catch (_) {
    }
    return 'Microphone permission was not granted';
  }

  private createCapturerOptions(): audio.AudioCapturerOptions {
    return {
      streamInfo: {
        samplingRate: this.sampleRate as audio.AudioSamplingRate,
        channels: audio.AudioChannel.CHANNEL_1,
        sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
        encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW,
      },
      capturerInfo: {
        source: audio.SourceType.SOURCE_TYPE_UNPROCESSED,
        capturerFlags: 0,
      }
    };
  }

  private async ensureCapturer(): Promise<string | null> {
    if (this.capturer !== null) {
      return null;
    }
    try {
      this.capturer = await audio.createAudioCapturer(this.createCapturerOptions());
    } catch (error) {
      return `Failed to create audio capturer: ${JSON.stringify(error)}`;
    }
    this.capturer.on('readData', (buffer: ArrayBuffer) => {
      this.xComponentContext.pushAudioFrame(buffer);
    });
    return null;
  }

  private async resetCapturer(): Promise<void> {
    if (this.capturer === null) {
      return;
    }
    try {
      if (this.started) {
        await this.capturer.stop();
      }
    } catch (_) {
    }
    try {
      await this.capturer.release();
    } catch (_) {
    }
    this.capturer = null;
    this.started = false;
  }

  async start(): Promise<void> {
    const permissionError: string | null = await this.ensureMicrophonePermission();
    if (permissionError !== null) {
      throw new Error(permissionError);
    }
    const capturerError: string | null = await this.ensureCapturer();
    if (capturerError !== null) {
      throw new Error(capturerError);
    }
    if (this.started) {
      return;
    }
    if (!this.xComponentContext.startDecoder(this.sampleRate, 1)) {
      throw new Error('Failed to start native decoder runtime');
    }
    this.xComponentContext.setDecoderMode(this.currentMode);
    try {
      await this.capturer!.start();
      this.started = true;
      AudioCaptureService.resumeAfterBackgroundFlag = false;
    } catch (err) {
      this.xComponentContext.stopDecoder();
      await this.resetCapturer();
      throw new Error(`Failed to start audio capturer: ${JSON.stringify(err)}`);
    }
  }

  setDecoderMode(mode: string): void {
    this.currentMode = mode;
    AudioCaptureService.lastSelectedMode = mode;
    this.xComponentContext.setDecoderMode(mode);
  }

  isRunning(): boolean {
    return this.started;
  }

  async pauseForBackground(): Promise<void> {
    AudioCaptureService.resumeAfterBackgroundFlag = this.started;
    if (!this.started || this.capturer === null) {
      return;
    }
    try {
      await this.capturer.stop();
    } finally {
      this.xComponentContext.stopDecoder();
      this.started = false;
    }
  }

  async resumeFromBackground(): Promise<boolean> {
    if (!AudioCaptureService.resumeAfterBackgroundFlag) {
      return false;
    }
    await this.start();
    return true;
  }

  async stop(): Promise<void> {
    AudioCaptureService.resumeAfterBackgroundFlag = false;
    this.xComponentContext.stopDecoder();
    if (this.capturer !== null && this.started) {
      await this.capturer.stop();
    }
    this.started = false;
  }

  async release(): Promise<void> {
    await this.stop();
    await this.resetCapturer();
  }
}
