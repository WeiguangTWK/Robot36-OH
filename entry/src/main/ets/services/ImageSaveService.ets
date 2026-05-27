import { common } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';

export class ImageSaveResult {
  status: string;
  path: string;
  message: string;

  constructor(status: string, path: string, message: string) {
    this.status = status;
    this.path = path;
    this.message = message;
  }
}

export class ImageSaveService {
  static buildSuggestedBmpName(timestamp: Date, modeName: string, imageIndex: number): string {
    const dateStamp = `${timestamp.getFullYear()}${ImageSaveService.padNumber(timestamp.getMonth() + 1)}${ImageSaveService.padNumber(timestamp.getDate())}`;
    const timeStamp = `${ImageSaveService.padNumber(timestamp.getHours())}${ImageSaveService.padNumber(timestamp.getMinutes())}${ImageSaveService.padNumber(timestamp.getSeconds())}`;
    const safeMode = ImageSaveService.sanitizeFileNameSegment(modeName);
    const safeIndex = ImageSaveService.padNumber(Math.max(imageIndex, 1));
    return `robot36_${dateStamp}_${timeStamp}_${safeMode}_${safeIndex}.bmp`;
  }

  static async saveBmp(context: common.UIAbilityContext, fileName: string, imageBuffer: ArrayBuffer): Promise<ImageSaveResult> {
    const documentPicker = new picker.DocumentViewPicker();
    const saveOptions = new picker.DocumentSaveOptions();
    saveOptions.newFileNames = [fileName];
    saveOptions.fileSuffixChoices = ['.bmp'];
    try {
      const savedUris = await documentPicker.save(saveOptions);
      if (savedUris.length < 1) {
        return new ImageSaveResult('cancelled', '', 'Save cancelled');
      }

      const targetPath = savedUris[0];
      const openedFile = fs.openSync(targetPath, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(openedFile.fd, imageBuffer);
      } finally {
        fs.closeSync(openedFile.fd);
      }
      return new ImageSaveResult('saved', targetPath, 'Saved image');
    } catch (error) {
      const message = ImageSaveService.extractErrorMessage(error);
      if (ImageSaveService.looksLikeCancel(message)) {
        return new ImageSaveResult('cancelled', '', 'Save cancelled');
      }
      throw new Error(`Failed to save image: ${message}`);
    }
  }

  private static sanitizeFileNameSegment(value: string): string {
    let result = '';
    for (let i = 0; i < value.length; i += 1) {
      const char = value.charAt(i);
      const code = value.charCodeAt(i);
      const isAlphaNumeric = (code >= 48 && code <= 57) || (code >= 65 && code <= 90) || (code >= 97 && code <= 122);
      if (isAlphaNumeric) {
        result += char;
        continue;
      }
      if (char === ' ' || char === '-' || char === '_') {
        result += '_';
      }
    }

    while (result.indexOf('__') >= 0) {
      result = result.replace('__', '_');
    }
    if (result.startsWith('_')) {
      result = result.substring(1);
    }
    if (result.endsWith('_')) {
      result = result.substring(0, result.length - 1);
    }
    return result.length > 0 ? result : 'Unknown_Mode';
  }

  private static extractErrorMessage(error: Object): string {
    if (error instanceof Error) {
      return error.message;
    }
    return JSON.stringify(error);
  }

  private static looksLikeCancel(message: string): boolean {
    const lowerMessage = message.toLowerCase();
    return lowerMessage.indexOf('cancel') >= 0;
  }

  private static padNumber(value: number): string {
    return value < 10 ? `0${value}` : `${value}`;
  }
}
