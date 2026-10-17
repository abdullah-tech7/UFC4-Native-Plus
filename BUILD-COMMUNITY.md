# UFC 4 community preview build

The shipped runtime is the measured UFC3Native v0.1.5 renderer/runtime plus the UFC4
extension, built with LLVM-MinGW 20261006 UCRT x86_64 (Clang 23.1.0) and Python 3.
The package uses its own embeddable Python at run time; system Python is not required.
Obtain the original GNSB ufc3-v0.1.5 Windows runtime release and retain its licenses.
The required bin/sacredfire-game.exe SHA-256 is:
3d2bcd9b6b59d349e73c03b940cec01e4589c972ed4f29b0e524335a3377651a

    python -B tools/build_ufc4.py --compiler PATH/TO/x86_64-w64-mingw32-clang.exe --donor PATH/TO/bin/sacredfire-game.exe --out build
    x86_64-w64-mingw32-clang -O2 -Wall -Wextra -Werror -municode -mwindows -Wl,--no-insert-timestamp tools/community_entry.c -lcomdlg32 -o build/UFC4.exe

Copy tools/ufc4_launcher.py to launcher.py beside UFC4.exe. Include scripts/{prepare.py,
link_modules.py,link_libc.py,content_profile.py,game_check.py,ufc4_profiles.json,save_snapshots.py},
tools/gen_import_names.py, share/aerolib.inl and the upstream generic black skip_movie.mkv;
include the donor python/, licenses/ and bin/{SDL3.dll,libc++.dll,libunwind.dll,libwinpthread-1.dll}.
Copy LICENSE and THIRD_PARTY_NOTICES.md. Do not ship game_path.txt, game data, cache,
logs or an existing user directory. The first-launch native selector writes game_path.txt.

Expected extension/runtime SHA-256 (before the independently compiled portable launcher):
0be83230a1cf90bf2ac296b3a54c766bf314be804687c5fc358920abf7a0ba6a
The complete renderer has not been independently rebuilt from its C++ source for this
preview. Changes to renderer C++ files require a renderer rebuild, not only the extension.
Inherited UFC 3 scripts/workflows are reference material; these are the UFC4 commands.
