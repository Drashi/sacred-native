# Sacred Native logo

`sacred-native-1024.png` is the project's logo (1024x1024 RGBA, transparent background); nothing of the game.

- `sacred-native-256.png` and `sacred-native-512.png` are the same picture made smaller, for the Linux bundle
  (`icons/`: the windows' icon, `platform/app_icon.h`, and the desktop entry, `tools/dist/linux/`). After a change to
  the logo make them again on a Mac:

      sips -z 512 512 data/icons/sacred-native-1024.png --out data/icons/sacred-native-512.png
      sips -z 256 256 data/icons/sacred-native-1024.png --out data/icons/sacred-native-256.png

- The macOS app icon is made from the logo by `tools/dist/make_bundle_macos.sh`: with Xcode 26 or newer an Icon
  Composer icon (`tools/dist/macos/AppIcon.icon.json`: the medallion on a dark rounded square, as macOS 26 wants
  every app icon) compiled by actool into `Assets.car` and `AppIcon.icns`; with an older Xcode the round logo alone,
  which macOS 26 puts on a grey square. The app's window and Dock icon
  (`Contents/Resources/icons/sacred-native-512.png`) is the round logo with the margin of macOS app icons.
