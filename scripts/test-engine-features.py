# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for nekodrop's link extraction and media capabilities patches."""
import json
import os
import sys
import unittest

if len(sys.argv) < 2:
    sys.exit('usage: test-engine-features.py <patched airdrop dir> [unittest args]')

sys.path.insert(0, sys.argv[1])
del sys.argv[1]

import receiver  # noqa: E402
import transfer  # noqa: E402


class LinkExtractionTests(unittest.TestCase):
    def test_plain_web_link(self):
        ask = {'TransferType': {'links': True}, 'Items': ['https://example.org/page']}
        self.assertEqual(transfer.extract_web_links(ask), ['https://example.org/page'])

    def test_bilibili_share_text_extracts_url(self):
        ask = {
            'TransferType': {'links': True},
            'Items': ['【实测 Windows 原生隔空投送】 https://b23.tv/abc1234）'],
        }
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/abc1234'])

    def test_items_without_transfer_type_when_no_files(self):
        ask = {'Items': ['https://www.bilibili.com/video/BV1xx411c7mD']}
        self.assertEqual(
            transfer.extract_web_links(ask),
            ['https://www.bilibili.com/video/BV1xx411c7mD'],
        )

    def test_dict_item_with_url(self):
        ask = {'Items': [{'Title': 'Video', 'URL': 'https://b23.tv/xyz999'}]}
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/xyz999'])

    def test_rejects_file_and_javascript_schemes(self):
        ask = {
            'TransferType': {'links': True},
            'Items': ['file:///C:/Windows/win.ini', 'javascript:alert(1)'],
        }
        self.assertEqual(transfer.extract_web_links(ask), [])

    def test_extracts_url_from_mixed_files_and_items(self):
        ask = {
            'TransferType': {'files': True},
            'Files': [{'FileName': 'cover.jpeg'}],
            'Items': ['【视频标题】 https://b23.tv/mixed123'],
        }
        self.assertEqual(transfer.extract_web_links(ask), ['https://b23.tv/mixed123'])

    def test_save_preview_icon_jpeg_and_rejects_junk(self):
        fake_jpg = b'\xff\xd8\xff\xe0' + b'\x00' * 32
        path = transfer.save_preview_icon({'FileIcon': fake_jpg}, 't1')
        self.assertTrue(path.endswith('.jpg'))
        self.assertTrue(os.path.isfile(path))
        os.unlink(path)
        self.assertEqual(transfer.save_preview_icon({'FileIcon': b'not an image at all!'}, 't2'), '')


class MediaCapabilitiesTests(unittest.TestCase):
    def tearDown(self):
        os.environ.pop('NEKODROP_MEDIA_MODE', None)

    def test_default_is_compatible_v1(self):
        os.environ.pop('NEKODROP_MEDIA_MODE', None)
        caps = json.loads(receiver._media_capabilities().decode())
        self.assertEqual(caps, {'Version': 1})

    def test_original_mode_advertises_heic_and_hevc(self):
        os.environ['NEKODROP_MEDIA_MODE'] = 'original'
        caps = json.loads(receiver._media_capabilities().decode())
        self.assertEqual(caps['Version'], 1)
        self.assertIn('hvc1', caps['Codecs'])
        self.assertIn('public.heic', caps['ContainerFormats'])


if __name__ == '__main__':
    unittest.main()
