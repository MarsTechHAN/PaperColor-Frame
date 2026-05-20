(function () {
  'use strict';

  const STORAGE_KEY = 'paperColor.lang';
  const LEGACY_STORAGE_KEY = 'paperE6.lang';
  const FALLBACK = 'en';
  const DICT = {
    en: {
      'app.device': 'PaperColor Frame',
      'app.wordmark': 'Photos',
      'app.wordmarkSub': 'Photo frame',
      'app.prints': 'photos',
      'app.settings': 'Settings',
      'app.galleryActions': 'Photo actions',

      'empty.aria': 'Upload first photo',
      'empty.title': 'Add a photo',
      'empty.note': 'Upload a photo to show it on the frame.',

      'action.next': 'Next photo',
      'action.upload': 'Upload photo',
      'action.cancel': 'Cancel',
      'action.done': 'Done',
      'action.close': 'Close',
      'action.back': 'Back',
      'action.delete': 'Delete',
      'action.tune': 'Adjust',
      'action.showNow': 'Show now',
      'action.apply': 'Apply',
      'action.auto': 'Auto adjust',
      'action.savePreset': 'Save preset',
      'action.send': 'Send to frame',
      'action.fit': 'Fit to screen',
      'action.rotate': 'Rotate 90 degrees',
      'action.nextAdjust': 'Next: Adjust',
      'action.showOnPanel': 'Show on frame',
      'action.skip': 'Skip',
      'action.save': 'Save',
      'action.nextArrow': 'Next',

      'upload.cropTitle': 'Crop',
      'upload.adjustTitle': 'Adjust',
      'upload.initialTitle': 'Crop and adjust',
      'upload.cropHint': 'Drag to move the photo. Pinch or scroll to zoom.',
      'upload.previewTitle': 'Compare the original photo with the processed preview',
      'upload.previewDithered': 'Processed preview. Tap to view the original photo.',
      'upload.previewSource': 'Original photo. Tap to view the processed preview.',
      'upload.previewLoading': 'Creating preview...',
      'adjust.manualTitle': 'Manual adjustment',
      'adjust.manualHint': 'Adjust brightness, color, and detail before sending. Drag a slider to change the value.',
      'adjust.brightness': 'Brightness',
      'adjust.exposure': 'Exposure',
      'adjust.contrast': 'Contrast',
      'adjust.saturation': 'Saturation',
      'adjust.vibrance': 'Color boost',
      'adjust.gamma': 'Midtones',
      'adjust.temperature': 'Color temperature',
      'adjust.tint': 'Color tint',
      'adjust.smoothness': 'Smoothing',
      'adjust.sharpen': 'Sharpness',
      'adjust.vignette': 'Corner brightness',
      'adjust.reset': 'Reset adjustments',

      'detail.photo': 'Photo',

      'settings.title': 'Settings',
      'settings.done': 'Done',
      'settings.introTitle': 'Frame settings',
      'settings.introText': 'Set playback, sleep, image processing, calibration, firmware, and language.',
      'settings.slideshow': 'Playback',
      'settings.loop': 'Auto change interval',
      'settings.loopHint': 'When playback is looping, the frame shows the next photo after this interval.',
      'settings.power': 'Power',
      'settings.wifiSleep': 'Sleep after Wi-Fi disconnects',
      'settings.wifiSleepHint': 'If no phone is connected to the frame Wi-Fi, the frame sleeps after this delay.',
      'settings.wifiSsid': 'Frame Wi-Fi',
      'settings.wifiSsidHint': 'Connect your phone to this Wi-Fi network before opening the control page.',
      'settings.wifiPassword': 'Wi-Fi password',
      'settings.wifiPasswordHint': 'Leave empty to use Wi-Fi without a password. Enter 8-63 characters to require a password after the next restart.',
      'settings.wifiOpen': 'No password',
      'settings.wifiProtected': 'Password set',
      'settings.wifiPasswordPlaceholder': 'Leave empty for no password',
      'settings.wifiPasswordChangePlaceholder': 'New password, or empty to remove it',
      'settings.saveWifiPassword': 'Save Wi-Fi settings',
      'settings.buttonSleep': 'Sleep after button refresh',
      'settings.buttonSleepHint': 'After the physical button refreshes the screen, the frame sleeps after this delay.',
      'settings.hardware': 'Hardware',
      'settings.ledBrightness': 'Status light brightness',
      'settings.ledBrightnessHint': 'Adjust the status light brightness. Set it to 0 to turn the light off.',
      'settings.imagePipeline': 'Image processing',
      'settings.ditherMode': 'Screen color conversion',
      'settings.ditherModeHint': 'Choose how photos are converted to screen colors. The mode affects color detail and grain.',
      'settings.ditherE6Mix': 'E6 Mix',
      'settings.ditherClassic': 'Classic',
      'settings.autoExposure': 'Auto exposure adjustment',
      'settings.autoExposureHint': 'Adjusts the brightness used by Auto. Higher values make the image brighter; lower values make it darker.',
      'settings.autoTemperature': 'Auto color temperature',
      'settings.autoTemperatureHint': 'Adjusts the color temperature used by Auto. Higher values look warmer; lower values look cooler.',
      'settings.library': 'Photo library',
      'settings.photos': 'Saved photos',
      'settings.photosHint': 'Photos saved on the device for playback or immediate display.',
      'settings.calibration': 'Screen color calibration',
      'settings.calibrationHint': 'Calibrate the screen colors so the preview better matches the actual display.',
      'settings.edit': 'Edit',
      'settings.firmware': 'Firmware',
      'settings.firmwareVersion': 'Firmware version',
      'settings.buildTime': 'Built: {time}',
      'settings.language': 'Language',
      'settings.languageHint': 'Choose the interface language. This setting is saved on the current phone.',
      'settings.languageEnglish': 'English',
      'settings.languageChinese': '中文',
      'settings.off': 'Off',
      'time.seconds': '{value} s',
      'time.minutes': '{value} min',
      'time.hours': '{value} h',

      'calib.title': 'Screen calibration',
      'calib.cancel': 'Cancel',
      'calib.currentPatch': 'Current patch',
      'calib.stepTitle': 'Calibration {step}/{total}',
      'calib.solidPatch': 'Solid {color} patch',
      'calib.mixPatch': 'Mixed patch: {mix}',
      'calib.requiredReading': 'Required reading',
      'calib.optionalReading': 'Optional reading',
      'calib.showHintRequired': 'After the frame finishes refreshing, enter the meter reading and save it.',
      'calib.showHintOptional': 'Enter and save the reading for a more accurate preview, or skip this patch.',
      'calib.inputMode': 'Reading format',
      'calib.rgbOption': 'RGB values',
      'calib.cmykOption': 'CMYK percentages',
      'calib.readingPlaceholder': 'RGB, for example 151, 161, 160',
      'calib.applyReading': 'Save reading',
      'calib.clearReading': 'Use estimate',
      'calib.nextPatch': 'Next patch',
      'calib.readingApplied': 'Reading saved',
      'calib.usingEstimate': 'Estimate applied',
      'calib.readoutSolid': 'Current color: {rgb}. If the screen looks different, enter and save a new meter reading.',
      'calib.readoutMeasured': 'Saved reading: {rgb}. Preview and uploads use this color.',
      'calib.readoutEstimated': 'Estimated color: {rgb}. You can skip this optional patch.',
      'calib.enterRgb': 'Enter 3 RGB values, for example 151, 161, 160.',
      'calib.enterCmyk': 'Enter 4 CMYK percentages, for example 5, 0, 0, 36.',
      'calib.sent': 'Patch sent. Wait for the screen to finish refreshing, then save the meter reading.',
      'calib.sendFailed': 'Could not send patch: {message}',
      'ink.black': 'black',
      'ink.white': 'white',
      'ink.yellow': 'yellow',
      'ink.red': 'red',
      'ink.blue': 'blue',
      'ink.green': 'green',

      'toast.refreshFailed': 'Refresh failed',
      'toast.deleted': 'Deleted',
      'toast.deleteFailed': 'Delete failed',
      'toast.refreshingDisplay': 'Refreshing screen...',
      'toast.showFailed': 'Could not show photo',
      'toast.cannotLoadPhoto': 'Could not load photo',
      'toast.nextPhoto': 'Showing next photo',
      'toast.autoTuning': 'Auto adjusting...',
      'toast.autoTuned': 'Auto adjusted: {preset}',
      'toast.presetSaved': 'Preset saved',
      'toast.cannotReadImage': 'Could not read image',
      'toast.encoding': 'Encoding...',
      'toast.dithering': 'Creating screen image...',
      'toast.uploaded': 'Uploaded. Refreshing screen...',
      'toast.uploadFailed': 'Upload failed',
      'toast.calibrationSaved': 'Calibration saved',
      'toast.saveFailed': 'Save failed',
      'toast.wasmFailed': 'Image processing module failed to load. Uploads will be slower.',
      'toast.sidecarStatus': 'Photo saved, but adjustment settings were not saved ({status})',
      'toast.sidecarFailed': 'Photo saved, but adjustment settings were not saved',
      'toast.wifiPasswordSaved': 'Wi-Fi password saved. Restart the frame to use it.',
      'toast.wifiPasswordCleared': 'Wi-Fi password removed after restart',
      'toast.wifiPasswordInvalid': 'Use no password, or enter 8-63 ASCII characters'
    },
    zh: {
      'app.device': 'PaperColor 相框',
      'app.wordmark': '照片',
      'app.wordmarkSub': '电子相框',
      'app.prints': '张照片',
      'app.settings': '设置',
      'app.galleryActions': '照片操作',

      'empty.aria': '上传第一张照片',
      'empty.title': '添加照片',
      'empty.note': '上传照片后，可以显示到相框上。',

      'action.next': '下一张',
      'action.upload': '上传照片',
      'action.cancel': '取消',
      'action.done': '完成',
      'action.close': '关闭',
      'action.back': '返回',
      'action.delete': '删除',
      'action.tune': '调整',
      'action.showNow': '立即显示',
      'action.apply': '应用',
      'action.auto': '自动调整',
      'action.savePreset': '保存预设',
      'action.send': '发送到相框',
      'action.fit': '适合屏幕',
      'action.rotate': '旋转 90 度',
      'action.nextAdjust': '下一步：调整',
      'action.showOnPanel': '显示到相框',
      'action.skip': '跳过',
      'action.save': '保存',
      'action.nextArrow': '下一个',

      'upload.cropTitle': '裁剪',
      'upload.adjustTitle': '调整',
      'upload.initialTitle': '裁剪和调整',
      'upload.cropHint': '拖动移动画面；双指或滚轮缩放。',
      'upload.previewTitle': '对比原图和处理后预览',
      'upload.previewDithered': '当前为处理后预览，点击查看原图。',
      'upload.previewSource': '当前为原图预览，点击查看处理后效果。',
      'upload.previewLoading': '正在生成预览...',
      'adjust.manualTitle': '手动调整',
      'adjust.manualHint': '发送前调整画面的明暗、颜色和细节。拖动滑块改变数值。',
      'adjust.brightness': '亮度',
      'adjust.exposure': '曝光',
      'adjust.contrast': '对比度',
      'adjust.saturation': '饱和度',
      'adjust.vibrance': '鲜艳度',
      'adjust.gamma': '中间调',
      'adjust.temperature': '色温',
      'adjust.tint': '色偏',
      'adjust.smoothness': '平滑',
      'adjust.sharpen': '锐度',
      'adjust.vignette': '边角亮度',
      'adjust.reset': '恢复默认调整',

      'detail.photo': '照片',

      'settings.title': '设置',
      'settings.done': '完成',
      'settings.introTitle': '相框设置',
      'settings.introText': '设置播放、休眠、图像处理、校准、固件和语言。',
      'settings.slideshow': '播放',
      'settings.loop': '自动切换间隔',
      'settings.loopHint': '循环播放时，相框会按这个间隔显示下一张照片。',
      'settings.power': '电源',
      'settings.wifiSleep': 'Wi-Fi 断开后休眠',
      'settings.wifiSleepHint': '手机没有连接相框 Wi-Fi 时，相框会在这段时间后休眠。',
      'settings.wifiSsid': '相框 Wi-Fi',
      'settings.wifiSsidHint': '打开控制页面前，先让手机连接这个 Wi-Fi。',
      'settings.wifiPassword': 'Wi-Fi 密码',
      'settings.wifiPasswordHint': '留空表示不设密码；填写 8-63 位字符后，重启后需要密码连接。',
      'settings.wifiOpen': '无密码',
      'settings.wifiProtected': '已设置密码',
      'settings.wifiPasswordPlaceholder': '留空表示不设密码',
      'settings.wifiPasswordChangePlaceholder': '输入新密码，或留空取消密码',
      'settings.saveWifiPassword': '保存 Wi-Fi 设置',
      'settings.buttonSleep': '按键刷新后休眠',
      'settings.buttonSleepHint': '按下实体按键刷新屏幕后，相框会在这段时间后休眠。',
      'settings.hardware': '硬件',
      'settings.ledBrightness': '状态灯亮度',
      'settings.ledBrightnessHint': '调节状态灯亮度。设为 0 时关闭。',
      'settings.imagePipeline': '图像处理',
      'settings.ditherMode': '屏幕颜色转换',
      'settings.ditherModeHint': '选择照片转换成屏幕颜色的方式。不同模式会影响颜色层次和颗粒感。',
      'settings.ditherE6Mix': 'E6 Mix',
      'settings.ditherClassic': 'Classic',
      'settings.autoExposure': '自动曝光调整',
      'settings.autoExposureHint': '调节自动处理后的明暗。数值越高画面越亮，越低画面越暗。',
      'settings.autoTemperature': '自动色温调整',
      'settings.autoTemperatureHint': '调节自动处理后的冷暖。数值越高画面越暖，越低画面越冷。',
      'settings.library': '照片库',
      'settings.photos': '已保存照片',
      'settings.photosHint': '保存在设备中的照片，可用于轮播或立即显示。',
      'settings.calibration': '屏幕颜色校准',
      'settings.calibrationHint': '校准屏幕颜色，让预览更接近实际显示效果。',
      'settings.edit': '编辑',
      'settings.firmware': '固件',
      'settings.firmwareVersion': '固件版本',
      'settings.buildTime': '编译时间：{time}',
      'settings.language': '语言',
      'settings.languageHint': '选择界面语言。设置会保存在当前手机上。',
      'settings.languageEnglish': 'English',
      'settings.languageChinese': '中文',
      'settings.off': '关闭',
      'time.seconds': '{value} 秒',
      'time.minutes': '{value} 分钟',
      'time.hours': '{value} 小时',

      'calib.title': '屏幕校准',
      'calib.cancel': '取消',
      'calib.currentPatch': '当前色块',
      'calib.stepTitle': '校准 {step}/{total}',
      'calib.solidPatch': '{color}色块',
      'calib.mixPatch': '混色色块：{mix}',
      'calib.requiredReading': '必填读数',
      'calib.optionalReading': '可选读数',
      'calib.showHintRequired': '相框刷新完成后，输入测量读数并保存。',
      'calib.showHintOptional': '需要更准确的预览时，输入读数并保存；也可以跳过。',
      'calib.inputMode': '读数格式',
      'calib.rgbOption': 'RGB 数值',
      'calib.cmykOption': 'CMYK 百分比',
      'calib.readingPlaceholder': 'RGB，例如 151, 161, 160',
      'calib.applyReading': '保存读数',
      'calib.clearReading': '使用估算值',
      'calib.nextPatch': '下一个色块',
      'calib.readingApplied': '读数已保存',
      'calib.usingEstimate': '已使用估算值',
      'calib.readoutSolid': '当前颜色：{rgb}。如果屏幕显示不一致，输入新的测量读数并保存。',
      'calib.readoutMeasured': '已保存读数：{rgb}。预览和上传会使用这个颜色。',
      'calib.readoutEstimated': '估算颜色：{rgb}。这个可选色块可以跳过。',
      'calib.enterRgb': '请输入 3 个 RGB 数值，例如 151, 161, 160。',
      'calib.enterCmyk': '请输入 4 个 CMYK 百分比，例如 5, 0, 0, 36。',
      'calib.sent': '色块已发送。等屏幕刷新完成后，保存测量读数。',
      'calib.sendFailed': '色块发送失败：{message}',
      'ink.black': '黑色',
      'ink.white': '白色',
      'ink.yellow': '黄色',
      'ink.red': '红色',
      'ink.blue': '蓝色',
      'ink.green': '绿色',

      'toast.refreshFailed': '刷新失败',
      'toast.deleted': '已删除',
      'toast.deleteFailed': '删除失败',
      'toast.refreshingDisplay': '正在刷新屏幕...',
      'toast.showFailed': '显示失败',
      'toast.cannotLoadPhoto': '无法加载照片',
      'toast.nextPhoto': '已显示下一张',
      'toast.autoTuning': '正在自动调整...',
      'toast.autoTuned': '已自动调整：{preset}',
      'toast.presetSaved': '预设已保存',
      'toast.cannotReadImage': '无法读取图片',
      'toast.encoding': '正在编码...',
      'toast.dithering': '正在生成屏幕图像...',
      'toast.uploaded': '已上传，正在刷新屏幕',
      'toast.uploadFailed': '上传失败',
      'toast.calibrationSaved': '校准已保存',
      'toast.saveFailed': '保存失败',
      'toast.wasmFailed': '图像处理模块加载失败，上传会变慢',
      'toast.sidecarStatus': '照片已保存，但调整参数未保存（{status}）',
      'toast.sidecarFailed': '照片已保存，但调整参数未保存',
      'toast.wifiPasswordSaved': 'Wi-Fi 密码已保存，重启后生效',
      'toast.wifiPasswordCleared': '重启后将取消 Wi-Fi 密码',
      'toast.wifiPasswordInvalid': '密码可留空；如需设置，请输入 8-63 位 ASCII 字符'
    }
  };

  function normalize(lang) {
    const v = String(lang || '').toLowerCase();
    if (v.startsWith('zh')) return 'zh';
    if (v.startsWith('en')) return 'en';
    return null;
  }

  function detect() {
    try {
      const saved = normalize(localStorage.getItem(STORAGE_KEY));
      if (saved) return saved;
      const legacy = normalize(localStorage.getItem(LEGACY_STORAGE_KEY));
      if (legacy) {
        localStorage.setItem(STORAGE_KEY, legacy);
        return legacy;
      }
    } catch (_) {}
    const langs = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language];
    for (const lang of langs) {
      const hit = normalize(lang);
      if (hit) return hit;
    }
    return FALLBACK;
  }

  let current = detect();

  function t(key, vars) {
    let out = (DICT[current] && DICT[current][key]) || DICT[FALLBACK][key] || key;
    if (vars) {
      out = out.replace(/\{(\w+)\}/g, (_, name) => vars[name] == null ? '' : String(vars[name]));
    }
    return out;
  }

  function apply(root) {
    const scope = root || document;
    document.documentElement.lang = current === 'zh' ? 'zh-CN' : 'en';
    scope.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
    scope.querySelectorAll('[data-i18n-title]').forEach(el => { el.title = t(el.dataset.i18nTitle); });
    scope.querySelectorAll('[data-i18n-placeholder]').forEach(el => { el.placeholder = t(el.dataset.i18nPlaceholder); });
    scope.querySelectorAll('[data-i18n-aria]').forEach(el => { el.setAttribute('aria-label', t(el.dataset.i18nAria)); });
    scope.querySelectorAll('[data-i18n-data-label]').forEach(el => { el.dataset.label = t(el.dataset.i18nDataLabel); });
    scope.querySelectorAll('[data-i18n-data-subtitle]').forEach(el => { el.dataset.subtitle = t(el.dataset.i18nDataSubtitle); });
    scope.querySelectorAll('[data-lang-option]').forEach(el => {
      const on = el.dataset.langOption === current;
      el.classList.toggle('is-active', on);
      el.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
  }

  function setLanguage(lang) {
    const next = normalize(lang) || FALLBACK;
    if (next === current) return;
    current = next;
    try { localStorage.setItem(STORAGE_KEY, current); } catch (_) {}
    apply(document);
    window.dispatchEvent(new CustomEvent('paper-i18n-change', {detail: {lang: current}}));
  }

  window.PaperI18n = { t, apply, setLanguage, getLanguage: () => current, languages: Object.keys(DICT) };

  document.addEventListener('click', e => {
    const btn = e.target.closest('[data-lang-option]');
    if (btn) setLanguage(btn.dataset.langOption);
  });

  apply(document);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => apply(document), {once: true});
  }
})();
