 <p align="center">
  <img src="https://img.shields.io/github/downloads/abdullah-tech7/UFC4-Native-Plus/total?style=for-the-badge&logo=github&label=Downloads" />
  <img src="https://img.shields.io/github/stars/abdullah-tech7/UFC4-Native-Plus?style=for-the-badge&logo=github&label=Stars" />
  <img src="https://img.shields.io/github/v/release/abdullah-tech7/UFC4-Native-Plus?style=for-the-badge&label=Latest%20Release" />
  <img src="https://img.shields.io/github/license/abdullah-tech7/UFC4-Native-Plus?style=for-the-badge&label=License" />
</p>

# 🥊 UFC4 Native Plus

A community-maintained fork of [UFC4-PC-Port](https://github.com/schans-arg/UFC4-PC-Port), bringing EA SPORTS UFC 4 to Windows with support for both US and EU game editions, performance and stability improvements, an optional DLC-related runtime patch, and a dedicated launcher.


UFC4 Native Plus reached its first stable release with v1.0.0. Subsequent updates continue to improve compatibility, performance, graphics, and gameplay customization.

This is an unofficial community project. It is not affiliated with, endorsed by, or sponsored by EA SPORTS, UFC, Sony, or PlayStation.

## ✨ Features

### 🎮 Game Compatibility

- **US and EU Edition Support:** Supports both North American and European game editions.
- **Game Update v28.00:** Targets the supported v28.00 update for both editions.
- **DLC Fighters in Career Mode:** Use DLC fighters in Career Mode without requiring separate DLC files.
- **Two-Controller Support:** Play locally with two controllers, with separate configurations for each player.

### 🖥️ Display & Resolution

- **Multiple Internal Resolutions:** Choose between 480p, 540p, 720p, 900p, 1080p, 1440p, and True 4K.
- **True 4K Support:** Render the game at native 4K internal resolution.
- **21:9 Ultrawide Support:** Added support for ultrawide displays.
- **PS4 Pro Resolution Handling:** Improved resolution handling for supported configurations.

### 🎨 Graphics Customization

Choose from three built-in graphics presets:

- **Low**
- **Medium**
- **High**
- **Custom:** Adjust individual graphics settings according to your preferences.

Available graphics options:

- **Crowd Density:** Low / Off, Medium (Less Details), and High (Full Details).
- **Shadow Quality:** Low / Off, Medium, and High.
- **Texture Quality:** Low, Medium, and High.
- **Motion Blur:** Enable or disable motion blur.

These settings provide more control over visual quality and performance, allowing users to reduce demanding effects based on their hardware.

### 🎮 Controller Customization

A complete controller remapping system with separate configurations for Player 1 and Player 2.

- **Full Button Remapping:** Customize controller buttons to match your preferred layout.
- **Independent Player Configurations:** Each player has their own separate button mappings.
- **In-Game Remapping:** Switch between custom and default mappings while playing, without navigating through the game menus.
- **Startup Configuration:** Automatically enable custom controller mappings when launching the game.
- **Button Icon Styles:** Choose between PlayStation and Xbox button icons displayed in-game.
- **Reset to Defaults:** Restore the original controller mappings.

**Local Two-Player Support**

Both controllers must be connected and turned on before launching the game. A second controller connected after the game has started will not be detected by the game engine.

### ⌨️ Keyboard Customization

A dedicated keyboard remapping system with customizable controls.

- **Custom Key Bindings:** Customize keys for different combat actions.
- **In-Game Remapping:** Switch between custom and default keyboard mappings while playing.
- **Automatic Key Swapping:** Prevent conflicts between strike and modifier keys.
- **Reset to Defaults:** Restore the original keyboard configuration.

### ⚡ Performance & Graphics Improvements

- **Performance Optimizations:** Improved overall performance and smoother gameplay.
- **Frame Pacing Improvements:** Reduced stuttering and improved frame pacing during fights.
- **Smoother Animations:** Improved the smoothness of strikes, hit animations, and visual effects.
- **Memory Optimization:** Improved memory usage across different GPU and VRAM configurations.
- **Low-End Hardware Support:** Includes lower internal resolution options and memory optimizations for less powerful systems.

### 🔊 Audio Improvements

- **Dialogue Boost:** Increased the center dialogue channel by +3 dB for clearer commentary, referee announcements, and fighter dialogue.
- **Adaptive Soft Limiter:** Helps reduce intense audio peaks and prevent digital distortion and clipping.
- **Audio Volume Control:** Adjust the audio volume directly from the launcher, with levels ranging from 75% to 200%.

### 🚀 Dedicated Launcher

- A dedicated launcher for configuring and starting the port.
- Resolution and graphics presets.
- Custom graphics settings.
- Controller and keyboard remapping.
- Separate controller configurations for Player 1 and Player 2.
- PlayStation and Xbox button icon selection.
- Audio volume control.
- Convenient setup and launch workflow.

## 📦 Release

See the [Releases](https://github.com/abdullah-tech7/UFC4-Native-Plus/releases) page for downloads and complete release notes.

## 📸 Screenshots

### 🎛️ Launcher

![UFC4 Native Plus Launcher](docs/images/n1.png)
![UFC4 Native Plus Launcher](docs/images/n2.png)
![UFC4 Native Plus Launcher](docs/images/n3.png)
![UFC4 Native Plus Launcher](docs/images/n4.png)
### 🥊 Fighter Selection

![Fighter selection](docs/images/u2.png)

### 👥 Fighter Roster

![Fighter roster](docs/images/u3.png)

### ⚖️ Fighter Comparison

![Fighter comparison](docs/images/u4.png)

### 🎮 Gameplay

![Gameplay](docs/images/u5.png)

## 🎯 Supported Game Editions

This project supports the following EA SPORTS UFC 4 Title IDs and game update version:

| Edition | Title ID | Required Update |
|---|---|---|
| US (North America) | `CUSA14204` | `v28.00` |
| EU (Europe) | `CUSA14209` | `v28.00` |

**Important:** Both editions require game update `v28.00`. Other update versions are not supported target configurations and may not work correctly.

## 💻 Requirements

- A legally obtained copy of EA SPORTS UFC 4 that you have extracted yourself.
- The relevant game executable/entry file requested by the launcher or port (`eboot.bin`).
- A compatible Windows system.
- Hardware capable of running the selected resolution and graphics settings.

The game is not included. Users must supply their own game files.

## 📁 Game Files & Optional DLC Patch

This repository does **not** include or distribute the game, game updates, DLC files, PlayStation firmware, save files, or other copyrighted game assets.

You must provide your own extracted copy of the base game.

The optional DLC-related patch is included in the port and does **not** require downloading or installing separate DLC files. The base game files are sufficient for the patch's intended DLC-unlocking behavior.

The patch changes how the port/runtime reports DLC availability; it does not contain the DLC assets themselves.

This technical description does not grant rights to DLC or other game content. Users are responsible for ensuring that their use complies with applicable law and relevant platform/game terms.

Do not upload game dumps, extracted game content, DLC files, saves, or compiled game images to issues, pull requests, or releases.

## 📥 Installation & Usage

1. Download the latest release archive from the [Releases](https://github.com/abdullah-tech7/UFC4-Native-Plus/releases) page.
2. Extract the archive to a folder of your choice.
3. Launch the included launcher.
4. Select the location of your own extracted UFC 4 game files when prompted.
5. Configure your resolution, graphics, audio, controller, and keyboard settings.
6. Start the port and enjoy UFC 4 on Windows.

Exact steps and available settings may change between releases. Follow any release-specific instructions included with the download.


## 🙏 Credits & Upstream

This project is a fork of **[schans-arg/UFC4-PC-Port](https://github.com/schans-arg/UFC4-PC-Port)**. Credit goes to the original project author and contributors for their original work and the foundation this fork builds upon.

The upstream project itself incorporates components from **UFC3Native v0.1.5**. Those upstream components and their authors must continue to receive the notices and attribution required by their applicable licenses.

Changes specific to this fork include:

- US and EU game edition support.
- Performance and stability improvements.
- Optional DLC-related runtime patch.
- Dedicated launcher.
- Career Mode DLC fighter support.
- Two-controller support.
- Audio improvements and dialogue boost.
- Additional internal resolutions and ultrawide support.
- Graphics customization and presets.
- Controller and keyboard remapping.
- Further fixes and improvements.

Please consult the commit history to distinguish upstream work from changes made in this fork.

## 📄 License & Third-Party Notices

This fork retains the upstream license and applicable copyright notices for inherited code.

The upstream runtime and integrated renderer are identified as GPL-2.0 in the upstream repository. See:

- [LICENSE](LICENSE)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- File-level license notices.

Third-party components may be governed by their own licenses. Their original license texts and notices must be preserved.

Do not assume that every file in this repository has identical licensing terms; consult the relevant file-level and third-party notices before redistributing components.

Unless a file or component states otherwise, changes to GPL-licensed portions are distributed under the terms required by GPL-2.0.

This source-code license does not grant rights to EA SPORTS UFC 4, its DLC, trademarks, or any other game assets.

## ⚠️ Disclaimer

EA SPORTS, UFC, PlayStation, and related names and marks belong to their respective owners.

This is an unofficial community project and is not affiliated with, endorsed by, or sponsored by those owners.

Use this software at your own risk. Compatibility is not guaranteed across all game editions, updates, hardware, or configurations.

## 🥊 Community & Support

Join the **Dynasty Team Discord** to stay updated on UFC4 Native Plus, get support, share feedback, and report bugs.

### 🔔 Updates & Announcements

Visit **#Update_UFC4-Native-Plus** for new releases, improvements, and project announcements.

### 🛠️ Support & Bug Reports

Visit **#Support_UFC4-Native-Plus** for troubleshooting, technical assistance, and bug reports.

### 💬 Join the Community

[Dynasty Team Discord](https://discord.gg/8qCwnGRpH)
