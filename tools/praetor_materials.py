"""Decode the packed native DOOM 2016 Praetor virtual-material export.

Channel assignments are verified in the local fragment shaders: normal X in
diffuse.a, normal Y in specular.a, smoothness=SRGBlinear(lightmap.a).
The existing hand shader uses a dielectric metallic/roughness approximation;
native specular RGB and scene lighting are not represented by that shader.
"""
import argparse
import pathlib

import numpy as np


def srgb_linear(value):
    return np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)


def decode_material(colour, specular, power):
    if any(image.dtype != np.uint8 or image.ndim != 3 or image.shape[2] != 4
           for image in (colour, specular, power)):
        raise ValueError('Expected RGBA8 input textures')
    if colour.shape != specular.shape or colour.shape != power.shape:
        raise ValueError('Texture dimensions differ')
    base = colour.copy()
    base[:, :, 3] = 255  # The original alpha is normal X, never opacity.
    x = colour[:, :, 3].astype(np.float32) / 127.5 - 1
    y = specular[:, :, 3].astype(np.float32) / 127.5 - 1
    z = np.sqrt(np.maximum(0, 1 - x*x - y*y))
    normal = np.empty_like(colour)
    normal[:, :, 0] = colour[:, :, 3]
    normal[:, :, 1] = specular[:, :, 3]
    normal[:, :, 2] = np.rint((z + 1) * 127.5).clip(0, 255).astype(np.uint8)
    normal[:, :, 3] = 255
    smoothness = srgb_linear(power[:, :, 3].astype(np.float32)/255)
    mr = np.full_like(colour, 255)
    mr[:, :, 1] = np.rint((1-smoothness)*255).clip(0,255).astype(np.uint8)
    mr[:, :, 2] = 0  # Native material uses specular/gloss, not a metallic mask.
    return base, normal, mr


def main():
    from PIL import Image
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    images = []
    for suffix in ('c', 's', 'g'):
        with Image.open(args.images / ('doommarine_playerhands_vmtr_'+suffix+'.png')) as image:
            images.append(np.array(image.convert('RGBA')))
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in zip(('BaseColor.png','Normal.png','MetallicRoughness.png'), decode_material(*images)):
        path = args.output / name
        Image.fromarray(data).save(path)
        print(path, data.shape[:2], flush=True)


if __name__ == '__main__':
    main()
