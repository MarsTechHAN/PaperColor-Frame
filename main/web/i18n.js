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
      'adjust.experimentalTitle': 'Experimental',
      'adjust.experimentalHint': 'Opt-in pipeline tweaks. Defaults are off; pick a variant only if you want to A/B compare.',
      'adjust.colorimetryMode': 'Color calibration mode',
      'adjust.colorimetryHint': 'SCE subtracts the AG glass first-surface reflection from the colorimeter readings. Stronger settings deepen blacks and lift mid-tone chroma but can crush shadow detail.',
      'adjust.colorimetry.legacy': 'Legacy (default)',
      'adjust.colorimetry.sceOff': 'SCE off (CSV Lab direct)',
      'adjust.colorimetry.sceSubtle': 'SCE subtle (k=0.02)',
      'adjust.colorimetry.sceDefault': 'SCE default (k=0.04)',
      'adjust.colorimetry.sceStrong': 'SCE strong (k=0.06)',

      'detail.photo': 'Photo',

      'settings.title': 'Settings',
      'settings.done': 'Done',
      'settings.introTitle': 'Frame settings',
      'settings.introText': 'Set playback, power, network, image processing, calibration, firmware, and language.',
      'settings.slideshow': 'Playback',
      'settings.loop': 'Auto change interval',
      'settings.loopHint': 'When playback is looping, the frame shows the next photo after this interval.',
      'settings.power': 'Power',
      'settings.wifiSleep': 'Sleep after Wi-Fi disconnects',
      'settings.wifiSleepHint': 'If no phone or STA network is connected, the frame sleeps after this delay.',
      'settings.network': 'Network',
      'settings.wifiMode': 'Wi-Fi mode',
      'settings.wifiModeHint': 'AP is the built-in hotspot. STA joins your router. STA+AP keeps both available after restart.',
      'settings.wifiModeAp': 'AP hotspot',
      'settings.wifiModeStaAp': 'STA + AP',
      'settings.wifiModeSta': 'STA only',
      'settings.wifiSsid': 'Frame Wi-Fi',
      'settings.wifiSsidHint': 'Built-in hotspot name used in AP and STA+AP modes.',
      'settings.wifiPassword': 'Wi-Fi password',
      'settings.wifiPasswordHint': 'Leave empty for an open hotspot. Enter 8-63 characters to protect AP mode after restart.',
      'settings.wifiOpen': 'No password',
      'settings.wifiProtected': 'Password set',
      'settings.wifiPasswordPlaceholder': 'Leave empty for no password',
      'settings.wifiPasswordChangePlaceholder': 'New password, or empty to remove it',
      'settings.saveWifiPassword': 'Save AP password',
      'settings.wifiStaSsid': 'Home Wi-Fi SSID',
      'settings.wifiStaSsidHint': 'Network name the frame joins in STA and STA+AP modes. Use printable ASCII characters.',
      'settings.wifiStaSsidPlaceholder': 'Router Wi-Fi name',
      'settings.wifiStaPassword': 'Home Wi-Fi password',
      'settings.wifiStaPasswordHint': 'Leave empty for an open network. Enter 8-63 characters for protected Wi-Fi.',
      'settings.wifiStaPasswordPlaceholder': 'Router password, or empty for open',
      'settings.wifiStaPasswordChangePlaceholder': 'New password, or empty to remove it',
      'settings.saveStaWifi': 'Save STA settings',
      'settings.mdns': 'Network address',
      'settings.mdnsHint': 'In STA mode, open this address from the same router network.',
      'settings.batteryIcon': 'Battery icon',
      'settings.batteryIconHint': 'Show a small battery indicator on photos. It turns red below 10%.',
      'settings.on': 'On',
      'settings.off': 'Off',
      'settings.hardware': 'Hardware',
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
      'settings.never': 'Never',
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
      'toast.wifiPasswordSaved': 'Wi-Fi settings saved. Restart the frame to use them.',
      'toast.wifiPasswordCleared': 'Wi-Fi password removed after restart',
      'toast.wifiPasswordInvalid': 'Use no password, or enter 8-63 ASCII characters',
      'toast.wifiSsidInvalid': 'Use 0-32 printable ASCII characters for SSID',
      'toast.networkRestart': 'Saved. Restart the frame to use the new network mode.',
      'toast.supportAddrCopied': 'Address copied. Paste it in Safari or Chrome.',
      'toast.supportAddrCopyFailed': 'Could not copy. Type the address into Safari or Chrome.',

      'support.title': 'Browser update needed',
      'support.heading': 'Open this page in Safari or Chrome',
      'support.lede': 'PaperColor Frame uses WebAssembly to crop and dither photos in your browser. The current browser does not support it, so Upload and Adjust cannot run here.',
      'support.step1': 'Open Safari or Chrome',
      'support.step1Hint': 'In-app browsers (WeChat, Instagram, X) often ship without WebAssembly. Use the share button to open the page in Safari or Chrome.',
      'support.step2': 'Visit the frame address',
      'support.step2Hint': 'Type one of these into the address bar. Tap to copy.',
      'support.addrMdnsLabel': 'mDNS name',
      'support.addrIpLabel': 'When joined to the frame Wi-Fi',
      'support.addrCopy': 'Copy',
      'support.addrCopied': 'Copied',
      'support.addrCopyFailed': 'Copy failed',
      'support.backToGallery': 'Back to gallery',

      'busy.working': 'Working…',
      'busy.saving': 'Saving…',
      'busy.encoding': 'Encoding photo…',
      'busy.dithering': 'Generating screen image…',
      'busy.uploadingJpg': 'Uploading photo…',
      'busy.uploadingBin': 'Uploading screen image…',
      'busy.sendingToFrame': 'Sending to frame…',
      'busy.loadingPhoto': 'Loading photo…',
      'busy.deleting': 'Deleting…',
      'busy.nextPhoto': 'Switching to next photo…'
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
      'adjust.experimentalTitle': '实验性',
      'adjust.experimentalHint': '可选的实验功能，默认关闭。仅当你想做 A/B 对比时再切换。',
      'adjust.colorimetryMode': '色彩校准模式',
      'adjust.colorimetryHint': 'SCE 会从测色仪读数中扣除 AG 玻璃表面反射。越强黑色越深、中调饱和度越高，但暗部可能压缩。',
      'adjust.colorimetry.legacy': '传统（默认）',
      'adjust.colorimetry.sceOff': 'SCE 关（直接用 CSV Lab）',
      'adjust.colorimetry.sceSubtle': 'SCE 轻度 (k=0.02)',
      'adjust.colorimetry.sceDefault': 'SCE 默认 (k=0.04)',
      'adjust.colorimetry.sceStrong': 'SCE 强 (k=0.06)',

      'detail.photo': '照片',

      'settings.title': '设置',
      'settings.done': '完成',
      'settings.introTitle': '相框设置',
      'settings.introText': '设置播放、电源、网络、图像处理、校准、固件和语言。',
      'settings.slideshow': '播放',
      'settings.loop': '自动切换间隔',
      'settings.loopHint': '循环播放时，相框会按这个间隔显示下一张照片。',
      'settings.power': '电源',
      'settings.wifiSleep': 'Wi-Fi 断开后休眠',
      'settings.wifiSleepHint': '手机或 STA 网络没有连接时，相框会在这段时间后休眠。',
      'settings.network': '网络',
      'settings.wifiMode': 'Wi-Fi 模式',
      'settings.wifiModeHint': 'AP 是相框热点；STA 连接路由器；STA+AP 重启后同时开启。',
      'settings.wifiModeAp': 'AP 热点',
      'settings.wifiModeStaAp': 'STA + AP',
      'settings.wifiModeSta': '仅 STA',
      'settings.wifiSsid': '相框 Wi-Fi',
      'settings.wifiSsidHint': 'AP 和 STA+AP 模式下使用的相框热点名称。',
      'settings.wifiPassword': 'Wi-Fi 密码',
      'settings.wifiPasswordHint': '留空表示 AP 热点无密码；填写 8-63 位字符后，重启生效。',
      'settings.wifiOpen': '无密码',
      'settings.wifiProtected': '已设置密码',
      'settings.wifiPasswordPlaceholder': '留空表示不设密码',
      'settings.wifiPasswordChangePlaceholder': '输入新密码，或留空取消密码',
      'settings.saveWifiPassword': '保存 AP 密码',
      'settings.wifiStaSsid': '家庭 Wi-Fi 名称',
      'settings.wifiStaSsidHint': 'STA 和 STA+AP 模式下，相框会连接这个路由器网络。请使用可打印 ASCII 字符。',
      'settings.wifiStaSsidPlaceholder': '路由器 Wi-Fi 名称',
      'settings.wifiStaPassword': '家庭 Wi-Fi 密码',
      'settings.wifiStaPasswordHint': '开放网络可留空；加密 Wi-Fi 请输入 8-63 位字符。',
      'settings.wifiStaPasswordPlaceholder': '路由器密码；开放网络留空',
      'settings.wifiStaPasswordChangePlaceholder': '输入新密码，或留空取消密码',
      'settings.saveStaWifi': '保存 STA 设置',
      'settings.mdns': '网络地址',
      'settings.mdnsHint': 'STA 模式下，在同一个路由器网络中打开这个地址。',
      'settings.batteryIcon': '电池图标',
      'settings.batteryIconHint': '在照片上显示小电池标识，低于 10% 时会变红。',
      'settings.on': '开',
      'settings.off': '关',
      'settings.hardware': '硬件',
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
      'settings.never': '永不',
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
      'toast.wifiPasswordSaved': 'Wi-Fi 设置已保存，重启后生效',
      'toast.wifiPasswordCleared': '重启后将取消 Wi-Fi 密码',
      'toast.wifiPasswordInvalid': '密码可留空；如需设置，请输入 8-63 位 ASCII 字符',
      'toast.wifiSsidInvalid': 'SSID 请使用 0-32 位可打印 ASCII 字符',
      'toast.networkRestart': '已保存，重启后使用新的网络模式',
      'toast.supportAddrCopied': '地址已复制，请到 Safari 或 Chrome 粘贴打开',
      'toast.supportAddrCopyFailed': '复制失败，请在 Safari 或 Chrome 中手动输入地址',

      'support.title': '请换个浏览器',
      'support.heading': '请在 Safari 或 Chrome 中打开',
      'support.lede': 'PaperColor 相框需要浏览器支持 WebAssembly 才能裁剪和抖色。当前浏览器不支持，无法继续上传或调整。',
      'support.step1': '切换到 Safari 或 Chrome',
      'support.step1Hint': '微信、Instagram、X 等应用内浏览器通常不支持 WebAssembly。请点击分享按钮，选择"在 Safari 中打开"或"在 Chrome 中打开"。',
      'support.step2': '访问相框地址',
      'support.step2Hint': '在地址栏输入下面任一地址，点击可复制。',
      'support.addrMdnsLabel': 'mDNS 域名',
      'support.addrIpLabel': '连接相框 Wi-Fi 时使用',
      'support.addrCopy': '复制',
      'support.addrCopied': '已复制',
      'support.addrCopyFailed': '复制失败',
      'support.backToGallery': '返回照片墙',

      'busy.working': '处理中…',
      'busy.saving': '保存中…',
      'busy.encoding': '正在编码…',
      'busy.dithering': '正在生成屏幕图像…',
      'busy.uploadingJpg': '上传照片中…',
      'busy.uploadingBin': '上传屏幕图像中…',
      'busy.sendingToFrame': '发送到相框…',
      'busy.loadingPhoto': '正在打开照片…',
      'busy.deleting': '正在删除…',
      'busy.nextPhoto': '切换下一张中…'
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
