from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SKIN = ROOT/'skin/zed'


class TopbarCoverTests(unittest.TestCase):
    def setUp(self):
        self.topbar = ET.parse(SKIN/'topbar.xml')

    def test_deck_selector_precedes_cover_and_track_metadata(self):
        row = next(node for node in self.topbar.iter('WidgetGroup') if node.findtext('ObjectName') == 'FonsBarra')
        children = list(row.find('Children'))
        self.assertEqual(children[0].findtext('ObjectName'), 'TabControls')
        self.assertEqual(children[1].findtext('ObjectName'), 'DeckFocus')
        self.assertEqual(children[2].findtext('ObjectName'), 'TopbarCoverStack')
        self.assertEqual(children[2].findtext('Size'), '60f,60f')
        self.assertEqual(children[3].findtext('ObjectName'), 'TopbarTrackStack')

    def test_both_decks_have_native_coverart(self):
        cover = next(node for node in self.topbar.iter('WidgetStack') if node.findtext('ObjectName') == 'TopbarCoverStack')
        self.assertEqual(cover.get('currentpage'), '[Skin],active_deck')
        self.assertEqual([node.findtext('Channel') for node in cover.iter('CoverArt')], ['1', '2'])
        self.assertEqual(cover.find('Children')[1].get('trigger'), '[Skin],active_deck')
        self.assertEqual(len(list(self.topbar.iter('CoverArt'))), 2)

    def test_deck_switch_and_performance_actions_remain(self):
        button = next(node for node in self.topbar.iter('PushButton') if node.findtext('ObjectName') == 'DeckFocus')
        self.assertEqual(button.findtext('Connection/ConfigKey'), '[Skin],active_deck')
        self.assertEqual(button.findtext('Size'), '48f,60f')
        self.assertEqual([node.findtext('Text') for node in button.findall('State')], ['1', '2'])
        names = {node.text for node in self.topbar.findall(".//SetVariable[@name='config_key']")}
        self.assertTrue({'library', 'hotcues', 'beatloop', 'keyshift', 'beatjump', 'stems'} <= names)

    def test_track_metadata_follows_cover_deck(self):
        stack = next(node for node in self.topbar.iter('WidgetStack') if node.findtext('ObjectName') == 'TopbarTrackStack')
        self.assertEqual(stack.get('currentpage'), '[Skin],active_deck')
        self.assertEqual([node.text for node in stack.findall(".//SetVariable[@name='channel']")], ['1', '2'])
        metadata = ET.parse(SKIN/'topbar_trackinfo.xml')
        self.assertEqual({node.findtext('Property') for node in metadata.iter('TrackProperty')},
                         {'titleInfo', 'artist', 'durationTextSeconds'})
        for node in metadata.iter('TrackProperty'):
            self.assertEqual(node.find('Channel/Variable').get('name'), 'channel')
        self.assertFalse(list(metadata.iter('CoverArt')))


if __name__ == '__main__':
    unittest.main()
