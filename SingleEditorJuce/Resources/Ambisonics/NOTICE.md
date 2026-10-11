# SADIE II derived Ambisonic binaural filters

`SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir` is a derived filter bank generated from:

- Dataset: SADIE Database, subject `SADIE_006`
- Source file: `SADIE_006_DFC_256_order_fir_48000.sofa`
- Source record: https://zenodo.org/records/12542676
- Source MD5: `05aaa22e2d5654cbdee5cbe83ebfb1ef`
- Copyright: University of York
- License: Apache License 2.0

The source dataset states that the original dataset must be referenced whenever it is used in original or modified form. The generated resource contains real diffuse-field-compensated HRIR data fitted into ACN/SN3D first-, second-, and third-order spherical-harmonic FIR banks.

Regenerate with:

```powershell
python tools\AmbisonicBinauralFilterTool.py `
  SADIE_006_DFC_256_order_fir_48000.sofa `
  SingleEditorJuce\Resources\Ambisonics\SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir
```

The generator verifies the pinned source MD5 before producing the resource.
