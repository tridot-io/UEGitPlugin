# tridot fork of UEGitPlugin

Branch `tridot/checkout-fetch-guard` = upstream `ProjectBorealis/UEGitPlugin` `dev` (30402ac, plugin 3.16, UE 5.8) + these patches:

- **Checkout guard** (`GitSourceControlOperations.cpp`, `EnsureFilesAtRemoteHead`): before `git lfs lock`, ask the server for the upstream branch head (`ls-remote <remote> <merge>`), fetch it if missing, and refuse the checkout if any requested file changed in `HEAD..<server head>` ("has a newer revision … Sync first."). Every failure refuses (server down, upstream misconfigured, multiple `merge` values). Only a branch with no upstream config or a detached HEAD skips the check, with a warning. Closes the window where the editor's 30 s background fetch has not run yet (editor start, right after a teammate's push).
- **`core.quotePath=false`** for git commands (not for the bundled `git-lfs.exe`), so non-ASCII asset paths match file states.

Known limits: a push landing between the server check and the lock is not caught; one `ls-remote` per checkout (~0.6 s on the office network); LFS locks for files inside submodules still go to the parent repository (upstream behaviour).

## Release (binaries for sites)

Sites do not build the plugin. Releases live in the company Gitea repo `Digital_Twin/UEGitPlugin`, branch `dist/ue<M>.<N>`, one commit + annotated tag per release (`v3.16-tridot.<n>+ue5.8`). Never delete or move a tag; site submodule pointers reference those commits.

1. Build from a clean checkout of this branch:
   `RunUAT.bat BuildPlugin -Plugin=<repo>\GitSourceControl.uplugin -Package=<out> -TargetPlatforms=Win64 -Rocket`
2. Assemble the release tree (binaries only, no `Source/`, no `.pdb`):
   - `GitSourceControl.uplugin` from `<out>` with `VersionName` = `3.16-tridot.<n>`
   - `Binaries/Win64/UnrealEditor-GitSourceControl.dll`, `Binaries/Win64/UnrealEditor.modules`
   - `Resources/Icon128.png`
   - `git-lfs.exe` from this repo root — **required**: lock/unlock run the plugin's own `git-lfs.exe` (`GIT_USE_CUSTOM_LFS`), and BuildPlugin does not package it
   - `LICENSE.txt` (MIT), `RELEASE.md` (source commit, engine BuildId)
   - `.gitattributes`: `* -text`, `*.dll` and `*.exe` as LFS
3. Commit on `dist/ue<M>.<N>` as the child of the previous release, tag, and `git push --atomic origin dist/ue<M>.<N> refs/tags/<tag>`.
4. Release again whenever the engine BuildId changes (`Engine/Binaries/Win64/UnrealEditor.modules`).

## Site use

```text
git submodule add -b dist/ue5.8 ../UEGitPlugin.git Plugins/GitSourceControl
git -C Plugins/GitSourceControl checkout v3.16-tridot.1+ue5.8
git add .gitmodules Plugins/GitSourceControl && git commit
```

Teammates: `git clone --recurse-submodules`, `git config submodule.recurse true`, `git lfs install`; close the editor before `git pull`. The project plugin overrides the engine's built-in Git plugin of the same name. Editor settings: Revision Control → provider "Git LFS 2", LFS locking on, LFS user name = the Gitea account.
