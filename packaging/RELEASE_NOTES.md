## This project has closed: use SMS Launcher

The features of this fork are going into the official [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) and **[SMS Launcher](https://github.com/chasem-dev/sms-launcher)** by chasem-dev, who made the port, as pull requests. SMS Launcher is the launcher to use from now on: it sets the game up from your own disc image and keeps it up to date.

### What this final version does
When you start the game, a single screen in SMS Launcher's own style replaces the launcher. **Get SMS Launcher** then:
1. **Backs up your memory card** to a dated `SMS PC Port save backup` folder in your home folder.
2. **Moves your saves.** If you kept them somewhere other than the usual folder (`save_dir`), they are copied to where SMS Launcher looks for them (`%APPDATA%\sms-port\card-a` on Windows, `~/.local/share/sms-port/card-a` on Linux). If you never changed it, they are already there. A memory card that SMS Launcher already has is never overwritten.
3. **Downloads the latest SMS Launcher** from its GitHub releases, with progress and Cancel, to your Downloads folder.
4. **Starts it**: the installer on Windows, or the AppImage on Linux.

Nothing is deleted: your disc image, mods and old saves stay where they are.

### Getting this version
- **On 1.4.0:** the launcher offers this update when it opens. Choose **Update now**.
- **On 1.3.0 or older:** download this release once and run it.

### Downloads
- **Windows (64-bit):** `SMS-PC-Port-*-windows-x64.zip`. Unzip anywhere and run `sms.exe`.
- **Linux (64-bit):** `SMS-PC-Port-*-linux-x86_64.AppImage`. Make it executable (`chmod +x`) and run it.

Thank you to everyone who played this fork. See you in SMS Launcher.
