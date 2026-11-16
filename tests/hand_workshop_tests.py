"""Opening Blender must never require a capture or launch the game."""
import sys
sys.dont_write_bytecode = True
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('tested_workshop',Path(__file__).resolve().parents[1]/'tools/hand_workshop.py')
work=importlib.util.module_from_spec(spec)
spec.loader.exec_module(work)
class WorkshopTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory=Path(self.temp.name)
        change=patch.multiple(work,OUT=self.directory,PROTECTED=False,WEAPON='heavy_assault_rifle')
        change.start()
        self.addCleanup(change.stop)
    def open(self):
        with patch.object(work,'baseline',return_value={}),patch.object(work,'settings'):
            work.main('open')
    def test_no_capture_builds_then_opens_blender_only(self):
        with patch.object(work,'run_blender') as build,patch.object(work.subprocess,'Popen') as launch:
            self.open()
        build.assert_called_once_with('build')
        launch.assert_called_once()
        self.assertEqual(launch.call_args.args[0][0],str(work.BLENDER))
        self.assertFalse((self.directory/'calibration-test').exists())
    def test_existing_artist_files_are_opened_unchanged(self):
        scene=self.directory/'atelier.blend'
        pose=self.directory/'pose.json'
        scene.write_bytes(b'artist scene')
        pose.write_bytes(b'artist pose')
        with patch.object(work,'run_blender') as build,patch.object(work.subprocess,'Popen') as launch:
            self.open()
        build.assert_not_called()
        launch.assert_called_once()
        self.assertEqual(scene.read_bytes(),b'artist scene')
        self.assertEqual(pose.read_bytes(),b'artist pose')
    def test_capture_package_refused_before_copy(self):
        with patch.object(work.shutil,'copytree') as copy:
            with self.assertRaisesRegex(ValueError,'retiree'):work.package(True)
        copy.assert_not_called()
    def test_old_capture_command_cannot_launch(self):
        with patch.object(work,'baseline',return_value={}),patch.object(work,'settings'),patch.object(work.subprocess,'Popen') as launch:
            with self.assertRaisesRegex(ValueError,'inconnue'):work.main('calibrate')
        launch.assert_not_called()
    def test_validated_weapon_cannot_be_reexported_or_repackaged(self):
        with patch.object(work,'PROTECTED',True),patch.object(work,'run_blender') as build,patch.object(work.shutil,'copy2') as copy:
            with self.assertRaisesRegex(ValueError,'validee'):work.main('prepare')
            with self.assertRaisesRegex(ValueError,'validee'):work.package()
        build.assert_not_called()
        copy.assert_not_called()
    def test_weapon_choice_reaches_background_blender(self):
        with patch.object(work.subprocess,'run') as run:
            work.run_blender('build')
        self.assertEqual(run.call_args.args[0][-2:],['--weapon','heavy_assault_rifle'])
    def test_failed_grip_check_blocks_package_before_copy(self):
        with patch.object(work,'baseline',return_value={}),patch.object(work,'settings'),patch.object(work,'run_blender') as export,patch.object(work,'verify_export',side_effect=ValueError('mauvais pivot')),patch.object(work,'package') as package:
            with self.assertRaisesRegex(ValueError,'mauvais pivot'):work.main('prepare')
        export.assert_called_once_with('export')
        package.assert_not_called()
    def test_validated_heavy_rifle_is_now_protected(self):
        self.assertTrue(work.WEAPONS['heavy_assault_rifle']['protected'])
    def test_plasma_reuses_exported_body_and_protects_validated_weapons(self):
        plasma=work.WEAPONS['plasma_rifle']
        self.assertTrue(plasma['protected'])
        self.assertEqual(plasma['meshes'],[1,2])
        self.assertEqual(plasma['attachments']['left'],{'joint':'base_jnt','index':1})
        self.assertTrue(work.WEAPONS['super_shotgun']['protected'])
        self.assertTrue(work.WEAPONS['heavy_assault_rifle']['protected'])
    def test_validated_rocket_workshop_is_protected(self):
        rocket=work.WEAPONS['rocket_launcher']
        self.assertTrue(rocket['protected'])
        self.assertEqual(rocket['stem'],'rocketlauncher')
        self.assertEqual(rocket['meshes'],[4])
        self.assertEqual(rocket['attachments']['left'],{'joint':'base_jnt','index':1})
        self.assertEqual(rocket['attachments']['right'],{'joint':'base_jnt','index':1})
        self.assertTrue(rocket['axes']['verified'])
    def test_validated_gauss_workshop_is_protected(self):
        gauss=work.WEAPONS['gauss_cannon']
        self.assertTrue(gauss['protected'])
        self.assertEqual(gauss['stem'],'gauss_rifle')
        self.assertEqual(gauss['meshes'],[1,2])
        self.assertEqual(gauss['attachments']['left'],{'joint':'handle_pivot_3_jnt','index':6})
        self.assertEqual(gauss['attachments']['right'],{'joint':'back_grip_jnt','index':2})
        self.assertTrue(gauss['axes']['verified'])
    def test_validated_chaingun_workshop_is_protected(self):
        chaingun=work.WEAPONS['chaingun']
        self.assertTrue(chaingun['protected'])
        self.assertEqual(chaingun['stem'],'chaingun')
        self.assertEqual(chaingun['meshes'],[0,2])
        self.assertEqual(chaingun['attachments']['left'],{'joint':'front_grip_jnt','index':7})
        self.assertEqual(chaingun['attachments']['right'],{'joint':'base_grip_adjust_jnt','index':2})
        self.assertTrue(chaingun['axes']['verified'])
    def test_validated_bfg_workshop_is_protected(self):
        bfg=work.WEAPONS['bfg']
        self.assertTrue(bfg['protected'])
        self.assertEqual(bfg['stem'],'bfg')
        self.assertEqual(bfg['meshes'],[4])
        for side in ('left','right'):
            self.assertEqual(bfg['attachments'][side],{'joint':'base_jnt','index':1})
        self.assertTrue(bfg['axes']['verified'])
    def test_validated_chainsaw_workshop_is_protected(self):
        chainsaw=work.WEAPONS['chainsaw']
        self.assertTrue(chainsaw['protected'])
        self.assertEqual(chainsaw['stem'],'chainsaw')
        self.assertEqual(chainsaw['meshes'],[0,1])
        for side in ('left','right'):
            self.assertEqual(chainsaw['attachments'][side],{'joint':'root','index':1})
        self.assertTrue(chainsaw['axes']['verified'])
    def test_validated_pistol_workshop_is_protected(self):
        pistol=work.WEAPONS['pistol']
        self.assertTrue(pistol['protected'])
        self.assertEqual(pistol['stem'],'pistol')
        self.assertEqual(pistol['meshes'],[0])
        for side in ('left','right'):
            self.assertEqual(pistol['attachments'][side],{'joint':'base_jnt','index':1})
        self.assertTrue(pistol['axes']['verified'])
if __name__=='__main__':unittest.main()
