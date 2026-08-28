*[Versión en español](INSTALL_GUIDE-es.md)*

# Installation guide (step by step, with screenshots)

This walks through the Windows installer end to end, including the Microsoft Edge SmartScreen
warning you'll see because the installer isn't code-signed yet (Early Access — see
`docs/ROADMAP.md`). Screenshots below are from a real install on an older version, before v0.3.1
added the Trigglow icon/wizard banner shown in the installer today — the steps and layout are
otherwise unchanged, only that generic styling and the version number in the corner are out of date.

## 1. Download

Get `obs-trigglow-dynamic-delay-0.3.2-windows-x64-setup.exe` from the
[latest release](https://github.com/VirosMs/obs-trigglow-dynamic-delay/releases/latest). Prefer to
install by hand instead? The `.zip` in the same release contains just the plugin files — see
"Installing the compiled plugin in OBS" in `README.md`.

## 2. Windows SmartScreen: why it appears, and how to get past it

Because the installer isn't signed with a code-signing certificate, Microsoft Defender SmartScreen
can't vouch for it and warns you before letting it run. This is expected for unsigned Early Access
software, not a sign that anything is wrong — but you should only click through it for installers
you actually trust the source of (in this case, the GitHub Releases page linked above).

Right after the download finishes, Edge shows this warning. Click **"Mantener de todos modos"**
("Keep anyway"):

![SmartScreen warning right after download](images/smartscreen-1-warning.png)

If you closed that dialog, or you're coming back to it later from the Downloads list, click the
**"..."** menu next to the file instead:

![Downloads list, "..." menu](images/smartscreen-2-menu.png)

...then choose **"Conservar"** ("Keep"):

![The "..." menu, Conservar/Keep option](images/smartscreen-3-keep.png)

Using a different browser? The warning and the exact button labels will look different (Chrome,
Firefox), but the underlying choice is the same: confirm you trust the file before opening it.

## 3. Close OBS before installing

**Install before you go live, not during a stream.** The installer needs to overwrite files that
OBS has open while it's running — if OBS is open when you start the install, it will ask to close it
automatically (see step 4 below) and relaunch it afterward. Plan the install for before a session,
not mid-broadcast.

## 4. Run the installer

It's a standard next-next-next wizard:

1. Pick a language:

   ![Language selector](images/installer-1-language.png)

2. Welcome screen — confirms the version being installed (shows 0.2.0 and Inno Setup's generic
   styling in this screenshot; as of v0.3.1 you'll see the Trigglow icon and banner instead, plus
   the current version number, but the same layout and buttons):

   ![Welcome screen, version 0.2.0](images/installer-2-welcome.png)

3. Destination folder — auto-detected from your OBS Studio install (via the registry key the
   official OBS installer writes); you can browse to a different folder if needed:

   ![Destination folder](images/installer-3-destination.png)

4. If OBS is still running, the installer offers to close it for you automatically:

   ![OBS Studio must be closed to continue](images/installer-4-close-obs.png)

5. Installing:

   ![Installing / restarting applications](images/installer-5-installing.png)

6. Done — optionally launch OBS Studio right away:

   ![Installation complete](images/installer-6-finish.png)

## 5. Enable the dock in OBS

The dock doesn't always show up automatically the first time. In OBS's menu bar, open
**Paneles (View → Docks)**:

![OBS menu bar, Paneles/Docks](images/obs-panel-1-menu.png)

Either click **"Restablecer paneles"** ("Reset docks") or just make sure **"Trigglow Dynamic
Delay"** is checked in that same menu:

![Docks menu with Trigglow Dynamic Delay listed](images/obs-panel-2-reset-panels.png)

The dock appears, showing `● Inactive` — you're ready to pick a live scene and go. See `README.md`
("Using the plugin") for what to configure next.

![Trigglow Dynamic Delay dock, Inactive state](images/obs-panel-3-dock.png)
