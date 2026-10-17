#!/usr/bin/env python3
"""UFC 3 Linux launcher: offline, join, or host a private online session."""
import os
from pathlib import Path
import subprocess
import sys
import threading

from ufc3_online import data_dir, load_settings, local_addresses, run_session, save_settings

try:
    import gi
    gi.require_version('Gtk', '4.0')
    from gi.repository import Gtk, Gio, GLib
except (ImportError, ValueError):
    sys.exit('UFC 3 launcher needs Python PyGObject and GTK 4. Install python-gobject + gtk4 (Arch), or python3-gi + gir1.2-gtk-4.0 (Debian/Ubuntu). CLI: ./ufc3.sh --play /path/to/game')

ROOT = Path(__file__).resolve().parent.parent
MODES = [('Offline', 'offline'), ('Join server', 'join'), ('Host server', 'host')]
LANGUAGES = [('English', '1'), ('French', '2'), ('Spanish', '3'), ('German', '4'),
             ('Italian', '5'), ('Japanese', '0'), ('Russian', '8')]
RESOLUTIONS = [('1280x720', '1280x720'), ('1920x1080', '1920x1080'),
               ('2560x1440', '2560x1440 (2K, experimental)'),
               ('3840x2160', '3840x2160 (4K, experimental)')]
CSS = """
window, window * { font-family: "Inter", "Cantarell", "Noto Sans", "DejaVu Sans", sans-serif; }
window, scrolledwindow, viewport { background-color: #0B0B0D; background-image: none; color: #F2F2F2; opacity: 1; }
label { color: #F2F2F2; }
.ufc-panel { background-color: #16161A; border: 1px solid #2A2A30; border-radius: 10px; padding: 14px; }
.ufc-section { color: #C9A24A; font-weight: 700; }
.ufc-banner-title { color: #C9A24A; font-size: 22px; font-weight: 700; }
.ufc-secondary { color: #A0A0A8; }
.ufc-error { color: #D20A0A; }
entry, combobox button, dropdown > button { background-color: #0F0F12; background-image: none; color: #F2F2F2; border: 1px solid #2A2A30; }
entry text, combobox popover contents, combobox cellview, dropdown popover contents, dropdown listview { background-color: #0F0F12; color: #F2F2F2; }
entry:focus-within, combobox button:focus, dropdown > button:focus { border-color: #C9A24A; }
button { background-color: #1E1E24; background-image: none; color: #F2F2F2; border: 1px solid #2A2A30; padding: 6px 12px; }
button:hover { border-color: #E8C872; }
checkbutton check { background-color: #16161A; border: 1px solid #2A2A30; }
checkbutton check:checked { background-color: #C9A24A; border-color: #C9A24A; }
.ufc-red { background-color: #D20A0A; border-color: #D20A0A; color: #F2F2F2; }
.ufc-red:hover { background-color: #E8C872; border-color: #E8C872; color: #0B0B0D; }
"""


class Launcher(Gtk.Application):
    def __init__(self):
        super().__init__(application_id='io.ufc3.NativeLauncher', flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.data = data_dir()
        self.settings = load_settings(self.data)
        if os.environ.get('BB_GAME_DIR'):
            self.settings['game'] = os.environ['BB_GAME_DIR']
        if not self.settings['game'] and (ROOT / 'orig/CUSA06534/eboot.bin').is_file():
            self.settings['game'] = str(ROOT / 'orig/CUSA06534')
        self.cancel = threading.Event()
        self.worker = None
        self.closing = False
        self.inputs = []

    def do_activate(self):
        self.window = Gtk.ApplicationWindow(application=self, title='EA SPORTS UFC 3')
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(self.window.get_display(), provider,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_USER + 1)
        self.window.set_default_size(900, 900)
        self.window.connect('close-request', self.close_request)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        for name in ('top', 'bottom', 'start', 'end'):
            getattr(box, 'set_margin_' + name)(20)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_child(box)
        self.window.set_child(scroll)
        self.banner_box = Gtk.Box(spacing=16)
        box.append(self.banner_box)
        self.refresh_banner()
        game_panel = self.section(box, 'Game')
        self.game = self.entry_row(game_panel, 'Game folder', self.settings['game'])
        browse = Gtk.Button(label='Choose game folder…')
        browse.connect('clicked', self.choose_folder)
        browse.set_halign(Gtk.Align.END)
        game_panel.append(browse)
        self.inputs.append(browse)
        self.player = self.entry_row(game_panel, 'Player name', self.settings['player'])
        self.player.set_max_length(16)
        self.language = self.combo_row(game_panel, 'Game language', LANGUAGES, self.settings['language'])
        online_panel = self.section(box, 'Online')
        self.mode = self.combo_row(online_panel, 'Online play', MODES, self.settings['mode'])
        self.mode.connect('changed', lambda *_: self.update_online())
        self.join = self.entry_row(online_panel, 'Join address', self.settings['join_address'])
        row = Gtk.Box(spacing=8)
        row.append(Gtk.Label(label='Host address', xalign=0, width_chars=15))
        self.host = Gtk.ComboBoxText.new_with_entry()
        self.host.set_hexpand(True)
        row.append(self.host)
        self.scan = Gtk.Button(label='Rescan')
        self.scan.connect('clicked', lambda *_: self.rescan())
        row.append(self.scan)
        self.copy = Gtk.Button(label='Copy IP')
        self.copy.connect('clicked', lambda *_: self.window.get_clipboard().set(self.host_ip()))
        row.append(self.copy)
        online_panel.append(row)
        self.inputs.extend([self.host, self.scan])
        self.rescan()
        help_text = Gtk.Label(label='Host starts the server with Play and stops it with the game.\nOutside your LAN: both PCs need a VPN (for example Tailscale).\nSelect this PC’s VPN IP here and share it with your friend.', xalign=0, wrap=True)
        help_text.add_css_class('ufc-secondary')
        online_panel.append(help_text)
        if 'BB_ONLINE_SERVER' in os.environ:
            note = Gtk.Label(label='BB_ONLINE_SERVER is set in your environment; it overrides Offline/Join. Host requires the same IP.', xalign=0, wrap=True)
            note.add_css_class('ufc-secondary')
            online_panel.append(note)
        video_panel = self.section(box, 'Video')
        self.resolution = self.combo_row(video_panel, 'Resolution', RESOLUTIONS, self.settings['resolution'])
        self.resolution.connect('changed', lambda *_: self.update_resolution())
        self.resolution_note = Gtk.Label(label='In game choose VIDEO -> Favor Resolution', xalign=0, wrap=True)
        self.resolution_note.add_css_class('ufc-secondary')
        video_panel.append(self.resolution_note)
        self.fps = self.combo_row(video_panel, 'FPS limit', [(x, x) for x in ('0', '30', '60', '90', '120', '144', '240')], self.settings['fps'])
        self.present = self.combo_row(video_panel, 'Present mode', [(x, x) for x in ('Mailbox', 'Fifo', 'Immediate')], self.settings['present'])
        self.checks = {}
        options = Gtk.Box(spacing=16)
        for label, key in [('Fullscreen', 'fullscreen'), ('PS4 Pro', 'neo')]:
            check = Gtk.CheckButton(label=label)
            check.set_active(self.settings[key])
            self.checks[key] = check
            self.inputs.append(check)
            options.append(check)
        video_panel.append(options)
        advanced_panel = self.section(box, 'Advanced')
        check = Gtk.CheckButton(label='Skip intro videos')
        check.set_active(self.settings['skip_movies'])
        self.checks['skip_movies'] = check
        self.inputs.append(check)
        advanced_panel.append(check)
        buttons = Gtk.Box(spacing=8)
        self.play = Gtk.Button(label='Play')
        self.play.add_css_class('ufc-red')
        self.play.connect('clicked', self.start)
        self.stop = Gtk.Button(label='Stop')
        self.stop.add_css_class('ufc-red')
        self.stop.set_sensitive(False)
        self.stop.connect('clicked', lambda *_: self.cancel.set())
        logs = Gtk.Button(label='Open logs')
        logs.connect('clicked', self.open_logs)
        for item in (self.play, self.stop, logs):
            buttons.append(item)
        box.append(buttons)
        self.status = Gtk.Label(label='Ready', xalign=0, wrap=True, selectable=True)
        box.append(self.status)
        self.update_online()
        self.update_resolution()
        self.window.present()

    def section(self, box, title):
        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        panel.add_css_class('ufc-panel')
        heading = Gtk.Label(label=title, xalign=0)
        heading.add_css_class('ufc-section')
        panel.append(heading)
        box.append(panel)
        return panel

    def refresh_banner(self):
        while True:
            child = self.banner_box.get_first_child()
            if child is None:
                break
            self.banner_box.remove(child)
        folder = self.game.get_text().strip() if hasattr(self, 'game') else self.settings['game'].strip()
        image = None
        if folder:
            sce_sys = Path(folder) / 'sce_sys'
            for name in ('icon0.png', 'pic1.png'):
                if (sce_sys / name).is_file():
                    image = Gtk.Picture.new_for_filename(str(sce_sys / name))
                    break
        if image is not None:
            image.set_size_request(-1, 120)
            image.set_can_shrink(True)
            image.set_content_fit(Gtk.ContentFit.SCALE_DOWN)
            self.banner_box.append(image)
        title = Gtk.Label(label='EA SPORTS UFC 3 — native PC port', xalign=0, valign=Gtk.Align.CENTER)
        title.add_css_class('ufc-banner-title')
        self.banner_box.append(title)

    def update_resolution(self):
        high = self.resolution.get_active_id() in ('2560x1440', '3840x2160')
        busy = bool(self.worker and self.worker.is_alive())
        self.resolution_note.set_visible(high)
        if high:
            self.checks['neo'].set_active(True)
        self.checks['neo'].set_sensitive(not busy and not high)

    def entry_row(self, box, label, text):
        row = Gtk.Box(spacing=8)
        row.append(Gtk.Label(label=label, xalign=0, width_chars=15))
        field = Gtk.Entry(text=text, hexpand=True)
        row.append(field)
        box.append(row)
        self.inputs.append(field)
        return field

    def combo_row(self, box, label, values, selected):
        row = Gtk.Box(spacing=8)
        row.append(Gtk.Label(label=label, xalign=0, width_chars=15))
        combo = Gtk.ComboBoxText(hexpand=True)
        for text, value in values:
            combo.append(value, text)
        combo.set_active_id(selected)
        if combo.get_active_id() is None:
            combo.set_active(0)
        row.append(combo)
        box.append(row)
        self.inputs.append(combo)
        return combo

    def host_ip(self):
        text = self.host.get_child().get_text().strip()
        return text.split()[0] if text else ''

    def rescan(self):
        current = self.host_ip() or self.settings['host_address']
        self.host.remove_all()
        addresses = local_addresses()
        for ip, name in addresses:
            self.host.append_text(f'{ip} ({name})')
        if current:
            self.host.get_child().set_text(current)
        elif addresses:
            self.host.set_active(0)

    def update_online(self):
        mode = self.mode.get_active_id()
        busy = bool(self.worker and self.worker.is_alive())
        self.join.set_sensitive(not busy and mode == 'join')
        self.host.set_sensitive(not busy and mode == 'host')
        self.scan.set_sensitive(not busy and mode == 'host')
        self.copy.set_sensitive(mode == 'host')

    def choose_folder(self, *_):
        dialog = Gtk.FileChooserNative(title='Choose CUSA06534 with update 1.14', transient_for=self.window,
                                       action=Gtk.FileChooserAction.SELECT_FOLDER, accept_label='Choose')
        def response(chooser, code):
            if code == Gtk.ResponseType.ACCEPT:
                self.game.set_text(chooser.get_file().get_path())
                self.refresh_banner()
            chooser.destroy()
        dialog.connect('response', response)
        dialog.show()

    def read(self):
        self.settings.update(game=self.game.get_text().strip(), player=self.player.get_text(),
                             language=self.language.get_active_id(), mode=self.mode.get_active_id(),
                             join_address=self.join.get_text().strip(), host_address=self.host_ip(),
                             fps=self.fps.get_active_id(), present=self.present.get_active_id(),
                             resolution=self.resolution.get_active_id())
        self.settings.update({key: check.get_active() for key, check in self.checks.items()})
        save_settings(self.data, self.settings)

    def notify(self, text, error=False):
        def update():
            self.status.set_text(text)
            if error:
                self.status.add_css_class('ufc-error')
            else:
                self.status.remove_css_class('ufc-error')
        GLib.idle_add(update)

    def start(self, *_):
        self.read()
        self.cancel.clear()
        self.play.set_sensitive(False)
        self.stop.set_sensitive(True)
        for item in self.inputs:
            item.set_sensitive(False)
        self.notify('Preparing session…')
        settings = dict(self.settings)
        def work():
            try:
                run_session(ROOT, self.data, settings, self.cancel, self.notify)
            except Exception as error:
                self.notify(str(error), error=True)
            finally:
                GLib.idle_add(self.finished)
        self.worker = threading.Thread(target=work, daemon=False)
        self.worker.start()

    def finished(self):
        if self.closing:
            self.window.destroy()
            self.quit()
            return
        self.play.set_sensitive(True)
        self.stop.set_sensitive(False)
        for item in self.inputs:
            item.set_sensitive(True)
        self.worker = None
        self.update_online()
        self.update_resolution()
        if self.cancel.is_set():
            self.status.set_text('Session stopped. Its server has been closed.')

    def open_logs(self, *_):
        logs = self.data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        Gio.AppInfo.launch_default_for_uri(logs.as_uri(), None)

    def close_request(self, *_):
        self.read()
        if self.worker and self.worker.is_alive():
            self.closing = True
            self.cancel.set()
            self.status.set_text('Closing the game and hosted server…')
            return True
        return False


if __name__ == '__main__':
    app = Launcher()
    raise SystemExit(app.run(sys.argv))
