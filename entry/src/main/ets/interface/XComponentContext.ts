export default interface XComponentContext {
  drawTestFrame(): void;
  renderDecoderFrame(): void;
  getNativeState(): string;
  setDebugLabel(label: string): void;
  startDecoder(sampleRate: number, channelCount: number): boolean;
  stopDecoder(): void;
  pushAudioFrame(buffer: ArrayBuffer): void;
  setDecoderMode(mode: string): void;
  exportLatestImageBmp(): ArrayBuffer | null;
}
