# dungeons2forlinux

Minecraft Dungeons II needs Microsoft Gaming Services, which Proton doesn't provide, so the game can't start or sign in on Linux. dungeons2forlinux adds a small replacement for it to the game. With it installed, the game starts, signs in to your Xbox account, plays online, and can link your Microsoft account from the in-game settings.

## Install

You need Minecraft Dungeons II installed through Steam and run with Proton.

1. Quit the game if it's running.
2. Download `dungeons2forlinux-*.tar.gz` from the [latest release](https://github.com/Puremin0rez/dungeons2forlinux/releases/latest), extract it, and run the installer from its folder:
   ```sh
   ./install.sh
   ```
3. Start the game from Steam. The first time, it shows a code: go to <https://www.microsoft.com/link>, enter the code, and sign in with the Microsoft account that owns your Xbox profile. You stay signed in after that.

To uninstall, run `./install.sh --uninstall`.

Builds of the latest changes, for testing before a release, are in the [`dev` pre-release](https://github.com/Puremin0rez/dungeons2forlinux/releases/tag/dev).

Not affiliated with or endorsed by Microsoft or Mojang.
