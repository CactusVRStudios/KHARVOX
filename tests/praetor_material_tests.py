import importlib.util
import pathlib
import unittest

import numpy as np

spec = importlib.util.spec_from_file_location('praetor_materials',
    pathlib.Path(__file__).resolve().parents[1]/'tools/praetor_materials.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MaterialTests(unittest.TestCase):
    def textures(self):
        colour = np.array([[[21,35,43,128],[60,70,80,204]]],dtype=np.uint8)
        specular = np.array([[[80,90,100,128],[20,30,40,179]]],dtype=np.uint8)
        power = np.array([[[0,255,0,0],[0,255,0,255]]],dtype=np.uint8)
        return colour,specular,power

    def test_colour_preserved_and_opaque(self):
        source = self.textures()
        base,_,_ = module.decode_material(*source)
        np.testing.assert_array_equal(base[:,:,:3],source[0][:,:,:3])
        self.assertTrue(np.all(base[:,:,3]==255))
        self.assertEqual(int(source[0][0,0,3]),128)

    def test_normal_channels_are_alpha_not_rgb(self):
        source = self.textures()
        _,normal,_ = module.decode_material(*source)
        np.testing.assert_array_equal(normal[:,:,0],source[0][:,:,3])
        np.testing.assert_array_equal(normal[:,:,1],source[1][:,:,3])
        self.assertEqual(int(normal[0,0,2]),255)
        # Quantized tangent normals should remain on the positive hemisphere.
        vectors=normal[:,:,:3].astype(float)/127.5-1
        np.testing.assert_allclose(np.sum(vectors*vectors,axis=2),1,atol=.015)

    def test_impossible_xy_is_clamped(self):
        source=self.textures();source[0][:,:,3]=255;source[1][:,:,3]=255
        _,normal,_=module.decode_material(*source)
        self.assertTrue(np.all(normal[:,:,2]==128))

    def test_roughness_extremes_ignore_green_placeholder(self):
        _,_,mr=module.decode_material(*self.textures())
        np.testing.assert_array_equal(mr[:,:,1],[[255,0]])
        self.assertTrue(np.all(mr[:,:,2]==0))

    def test_smoothness_uses_native_srgb_transfer(self):
        source=self.textures();source[2][:,:,3]=128
        _,_,mr=module.decode_material(*source)
        self.assertEqual(int(mr[0,0,1]),200)

    def test_dimensions_and_format_rejected(self):
        source=self.textures()
        with self.assertRaises(ValueError):module.decode_material(source[0][:,:1],*source[1:])
        with self.assertRaises(ValueError):module.decode_material(source[0].astype(float),*source[1:])


if __name__ == '__main__':
    unittest.main()
