from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import zed_session


class SessionTests(unittest.TestCase):
    def test_macro_catalog_requires_both_effects_and_uses_real_indices(self):
        choices = [(9, 'Filter'), (21, 'Echo')]
        macros = {item[0]: item for item in zed_session.macro_choices(choices)}
        self.assertEqual(macros[1], (1, 'Filter Echo', 9, 21))
        self.assertEqual(macros[10][2:], (9, 0))
        self.assertNotIn(8, macros)
        self.assertNotIn(3, macros)

    def test_macro_preset_order_and_visibility_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'org-mixxx-effects-echo.xml'
            identifiers = zed_session.PRESET_PARAMETERS['echo']
            def preset(names, hidden='0'):
                return '<Effect><Id>org.mixxx.effects.echo</Id><BackendType>Built-In</BackendType><Parameters>'+''.join(
                    '<Parameter><Id>'+name+'</Id><Hidden>'+hidden+'</Hidden></Parameter>' for name in names)+'</Parameters></Effect>'
            self.assertFalse(zed_session.compatible_preset('echo', directory))
            path.write_text(preset(identifiers))
            self.assertTrue(zed_session.compatible_preset('echo', directory))
            path.write_text(preset(reversed(identifiers)))
            self.assertFalse(zed_session.compatible_preset('echo', directory))
            path.write_text(preset(identifiers, '1'))
            self.assertFalse(zed_session.compatible_preset('echo', directory))

    def test_effect_catalog_uses_actual_order_not_fixed_indices(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'effects.xml'
            path.write_text('<MixxxEffects><VisibleEffects>'+''.join(
                '<Effect><Id>org.mixxx.effects.'+name+'</Id><BackendType>Built-In</BackendType></Effect>'
                for name in ('whitenoise', 'echo', 'filter', 'reverb'))+'</VisibleEffects></MixxxEffects>')
            self.assertEqual(zed_session.effect_choices(path), [(3, 'Filter'), (2, 'Echo'), (4, 'Reverb')])

    def test_missing_or_untrusted_catalog_is_not_guessed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'effects.xml'
            self.assertEqual(zed_session.effect_choices(path), [])
            for xml in ('invalid', '<MixxxEffects/>',
                    '<MixxxEffects><VisibleEffects><Effect><Id>x</Id><BackendType>LV2</BackendType></Effect></VisibleEffects></MixxxEffects>'):
                path.write_text(xml)
                self.assertEqual(zed_session.effect_choices(path), [])

    def test_wayland_selects_available_socket_before_qt_startup(self):
        runtime = str(Path.cwd())
        with patch.dict(zed_session.os.environ, {'XDG_RUNTIME_DIR': runtime}, clear=True), \
                patch.object(Path, 'is_socket', lambda path: path.name == 'wayland-1'):
            self.assertEqual(zed_session.prepare_wayland(timeout=0), 'wayland-1')
            self.assertEqual(zed_session.os.environ['WAYLAND_DISPLAY'], 'wayland-1')

    def test_wayland_does_not_replace_explicit_display(self):
        environment = {'XDG_RUNTIME_DIR': str(Path.cwd()), 'WAYLAND_DISPLAY': 'wayland-custom'}
        with patch.dict(zed_session.os.environ, environment, clear=True), \
                patch.object(Path, 'is_socket', lambda path: path.name == 'wayland-0'):
            with self.assertRaisesRegex(RuntimeError, 'timeout'):
                zed_session.prepare_wayland(timeout=0)
            self.assertEqual(zed_session.os.environ['WAYLAND_DISPLAY'], 'wayland-custom')

    def test_wayland_ambiguous_socket_selection_fails_closed(self):
        with patch.dict(zed_session.os.environ, {'XDG_RUNTIME_DIR': str(Path.cwd())}, clear=True), \
                patch.object(Path, 'is_socket', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'Multiple'):
                zed_session.prepare_wayland(timeout=0)

    def test_wayland_waits_for_delayed_socket(self):
        environment = {'XDG_RUNTIME_DIR': str(Path.cwd()), 'WAYLAND_DISPLAY': 'wayland-0'}
        with patch.dict(zed_session.os.environ, environment, clear=True), \
                patch.object(Path, 'is_socket', side_effect=[False, True]), \
                patch.object(zed_session.time, 'monotonic', return_value=0), \
                patch.object(zed_session.time, 'sleep') as sleep:
            self.assertEqual(zed_session.prepare_wayland(), 'wayland-0')
            sleep.assert_called_once_with(0.1)

    def test_wayland_requires_runtime_environment(self):
        for environment in ({}, {'XDG_RUNTIME_DIR': 'relative'}):
            with patch.dict(zed_session.os.environ, environment, clear=True):
                with self.assertRaisesRegex(RuntimeError, 'XDG_RUNTIME_DIR'):
                    zed_session.prepare_wayland(timeout=0)

    def test_wayland_regular_file_is_not_accepted_as_socket(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary)/'wayland-0').write_text('not a socket')
            with patch.dict(zed_session.os.environ, {'XDG_RUNTIME_DIR': temporary}, clear=True):
                with self.assertRaisesRegex(RuntimeError, 'timeout'):
                    zed_session.prepare_wayland(timeout=0)

    @unittest.skipUnless(zed_session.os.name == 'posix', 'Linux Unix socket test')
    def test_wayland_detects_real_unix_socket_without_connecting(self):
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            with zed_session.socket.socket(zed_session.socket.AF_UNIX, zed_session.socket.SOCK_STREAM) as server:
                server.bind(str(Path(temporary)/'wayland-1'))
                with patch.dict(zed_session.os.environ, {'XDG_RUNTIME_DIR': temporary}, clear=True):
                    self.assertEqual(zed_session.prepare_wayland(timeout=0), 'wayland-1')

    def test_only_exact_open_request_is_accepted(self):
        message = dict(type='control_change', channel=0, control=120, value=127)
        self.assertTrue(zed_session.is_open_request(SimpleNamespace(**message)))
        for field, value in (('type', 'note_on'), ('channel', 1), ('control', 10), ('value', 0)):
            self.assertFalse(zed_session.is_open_request(SimpleNamespace(**dict(message, **{field: value}))))

    def test_notify_abstract_socket(self):
        with patch.dict(zed_session.os.environ, {'NOTIFY_SOCKET': '@test-notify'}), \
                patch.object(zed_session.socket, 'AF_UNIX', 1, create=True), \
                patch.object(zed_session.socket, 'socket') as factory:
            zed_session.notify_ready()
            connection = factory.return_value.__enter__.return_value
            connection.connect.assert_called_once_with('\0test-notify')
            connection.sendall.assert_called_once_with(b'READY=1')

    def test_no_notify_socket_means_no_io(self):
        with patch.dict(zed_session.os.environ, {}, clear=True), \
                patch.object(zed_session.socket, 'socket') as factory:
            zed_session.notify_ready()
            factory.assert_not_called()


if __name__ == '__main__':
    unittest.main()
