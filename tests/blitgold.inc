// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// what the blitters draw, as a hash for each case: written by `test_blitgold print` (tests/blitgold.c)
	0x2ada318df0d9d7f1ull, // alpha copy rgb <- rgb, level 0x0
	0x84d69fdd7b1613acull, // alpha copy rgb <- argb, level 0x0
	0xa36a1c4e2ef2e605ull, // alpha copy argb <- rgb, level 0x0
	0x0bdf45a1c82ab18cull, // alpha copy argb <- argb, level 0x0
	0x2ada318df0d9d7f1ull, // blend rgb <- rgb, level 0x1
	0xe37cefb2b1a57f25ull, // blend rgb <- rgb, level 0x2
	0xdc6455b8e512e31eull, // blend rgb <- rgb, level 0x3f
	0x15abfd55322ee7b5ull, // blend rgb <- rgb, level 0x80
	0x465644b9df99ccc7ull, // blend rgb <- rgb, level 0x81
	0x4f5a299fd0bc03b9ull, // blend rgb <- rgb, level 0xfe
	0xfcd52df090357d79ull, // blend rgb <- rgb, level 0xff
	0x2c11c94096cd67b5ull, // blend rgb <- argb, level 0x1
	0x5a764c0701899407ull, // blend rgb <- argb, level 0x2
	0xa1335431837b9be2ull, // blend rgb <- argb, level 0x3f
	0x954066ec5553644full, // blend rgb <- argb, level 0x80
	0x72ed32c1875d24a9ull, // blend rgb <- argb, level 0x81
	0x05bdfe2f2c6a5ff0ull, // blend rgb <- argb, level 0xfe
	0x6aee09302afdac21ull, // blend rgb <- argb, level 0xff
	0xd965c10e6d10985dull, // blend argb <- rgb, level 0x1
	0x17fd6d8a3477c3e6ull, // blend argb <- rgb, level 0x2
	0x70168ad9c3e81c42ull, // blend argb <- rgb, level 0x3f
	0x42a9a06f0fb67de5ull, // blend argb <- rgb, level 0x80
	0x33f3a85fd7a75cabull, // blend argb <- rgb, level 0x81
	0xf2fd39895c60d15full, // blend argb <- rgb, level 0xfe
	0x7501673747efce84ull, // blend argb <- rgb, level 0xff
	0xfaa0d42004a50c22ull, // blend argb <- argb, level 0x1
	0xd4fd71bc5b832f59ull, // blend argb <- argb, level 0x2
	0xbbb6a049c0c49d49ull, // blend argb <- argb, level 0x3f
	0x2debb47dd15bfc16ull, // blend argb <- argb, level 0x80
	0x76003507c1964080ull, // blend argb <- argb, level 0x81
	0x77cac6b83718f2baull, // blend argb <- argb, level 0xfe
	0x6e32da55eec0b93dull, // blend argb <- argb, level 0xff
	0x2ada318df0d9d7f1ull, // over rgb <- rgb, level 0x1
	0xe37cefb2b1a57f25ull, // over rgb <- rgb, level 0x2
	0xdc6455b8e512e31eull, // over rgb <- rgb, level 0x3f
	0x15abfd55322ee7b5ull, // over rgb <- rgb, level 0x80
	0x465644b9df99ccc7ull, // over rgb <- rgb, level 0x81
	0x4f5a299fd0bc03b9ull, // over rgb <- rgb, level 0xfe
	0xfcd52df090357d79ull, // over rgb <- rgb, level 0xff
	0x2ada318df0d9d7f1ull, // over rgb <- argb, level 0x1
	0xe37cefb2b1a57f25ull, // over rgb <- argb, level 0x2
	0xdc6455b8e512e31eull, // over rgb <- argb, level 0x3f
	0x15abfd55322ee7b5ull, // over rgb <- argb, level 0x80
	0x465644b9df99ccc7ull, // over rgb <- argb, level 0x81
	0x4f5a299fd0bc03b9ull, // over rgb <- argb, level 0xfe
	0xfcd52df090357d79ull, // over rgb <- argb, level 0xff
	0x2ada318df0d9d7f1ull, // over argb <- rgb, level 0x1
	0xe37cefb2b1a57f25ull, // over argb <- rgb, level 0x2
	0xdc6455b8e512e31eull, // over argb <- rgb, level 0x3f
	0x15abfd55322ee7b5ull, // over argb <- rgb, level 0x80
	0x465644b9df99ccc7ull, // over argb <- rgb, level 0x81
	0x4f5a299fd0bc03b9ull, // over argb <- rgb, level 0xfe
	0xfcd52df090357d79ull, // over argb <- rgb, level 0xff
	0x2ada318df0d9d7f1ull, // over argb <- argb, level 0x1
	0xe37cefb2b1a57f25ull, // over argb <- argb, level 0x2
	0xdc6455b8e512e31eull, // over argb <- argb, level 0x3f
	0x15abfd55322ee7b5ull, // over argb <- argb, level 0x80
	0x465644b9df99ccc7ull, // over argb <- argb, level 0x81
	0x4f5a299fd0bc03b9ull, // over argb <- argb, level 0xfe
	0xfcd52df090357d79ull, // over argb <- argb, level 0xff
	0xf64997e12ccf2b24ull, // add rgb <- rgb, level 0x1
	0xfc97c882a708b5e4ull, // add rgb <- rgb, level 0x40
	0xd58220661d1dc1c8ull, // add rgb <- rgb, level 0x7f
	0x4a1c5184778176adull, // add rgb <- rgb, level 0x80
	0x474c9c5e81c91b29ull, // add rgb <- rgb, level 0xff
	0xdddf9fb1f0795c9bull, // add rgb <- rgb, level 0x100
	0x96afae5810452364ull, // add rgb <- rgb, level 0x101
	0xe74ff885a5be9255ull, // add rgb <- rgb, level 0x1c0
	0xf64997e12ccf2b24ull, // add rgb <- argb, level 0x1
	0xf8b246a904479fd1ull, // add rgb <- argb, level 0x40
	0x9d450a356357d5feull, // add rgb <- argb, level 0x7f
	0xf25bb88693c0309full, // add rgb <- argb, level 0x80
	0x0e70d1c71d67d88bull, // add rgb <- argb, level 0xff
	0xe5bbe8faca2052e2ull, // add rgb <- argb, level 0x100
	0x27f5b317d904a2d4ull, // add rgb <- argb, level 0x101
	0x459bb6f99e8a9c29ull, // add rgb <- argb, level 0x1c0
	0x72a56b7e9aec9da3ull, // add argb <- rgb, level 0x1
	0xc1b443a2ae7ccc99ull, // add argb <- rgb, level 0x40
	0xa2e6da18241e7a27ull, // add argb <- rgb, level 0x7f
	0x63da01377d387157ull, // add argb <- rgb, level 0x80
	0x48517479cee0bdc8ull, // add argb <- rgb, level 0xff
	0xa4a47fb185960180ull, // add argb <- rgb, level 0x100
	0x8d98b85c7548bde4ull, // add argb <- rgb, level 0x101
	0xa619a60e6fee6a34ull, // add argb <- rgb, level 0x1c0
	0xf64997e12ccf2b24ull, // add argb <- argb, level 0x1
	0xcb774b7073d61ab2ull, // add argb <- argb, level 0x40
	0x26f416f207d34d6dull, // add argb <- argb, level 0x7f
	0xa322971d8f8b3ff0ull, // add argb <- argb, level 0x80
	0xe9c9751007306801ull, // add argb <- argb, level 0xff
	0x9618dd386fa3a3c9ull, // add argb <- argb, level 0x100
	0xe443bb96a1910db7ull, // add argb <- argb, level 0x101
	0x1f2809dfbe3f60baull, // add argb <- argb, level 0x1c0
	0xf64997e12ccf2b24ull, // screen rgb <- rgb, level 0x1
	0xdf0525098b98191dull, // screen rgb <- rgb, level 0x40
	0x7823d5b8317a109full, // screen rgb <- rgb, level 0x7f
	0xc4a131c64acf47b1ull, // screen rgb <- rgb, level 0x80
	0x619786103e7ee855ull, // screen rgb <- rgb, level 0xff
	0x9c39d6df10d0bd9aull, // screen rgb <- rgb, level 0x100
	0xbfb81532f34cf553ull, // screen rgb <- rgb, level 0x101
	0x0c4fd204e3a49567ull, // screen rgb <- rgb, level 0x1c0
	0xf64997e12ccf2b24ull, // screen rgb <- argb, level 0x1
	0xa995a0eaa12c0917ull, // screen rgb <- argb, level 0x40
	0x2871d46ec3a92281ull, // screen rgb <- argb, level 0x7f
	0xe190e1651acc7e54ull, // screen rgb <- argb, level 0x80
	0x1f19503214cb24dcull, // screen rgb <- argb, level 0xff
	0xf46146562d67bf2aull, // screen rgb <- argb, level 0x100
	0x85da2eaca27ce206ull, // screen rgb <- argb, level 0x101
	0xc9086dc945e41d46ull, // screen rgb <- argb, level 0x1c0
	0xf64997e12ccf2b24ull, // screen argb <- rgb, level 0x1
	0x8ed0351a4b7e60d9ull, // screen argb <- rgb, level 0x40
	0xf2ae96fdfe514a7dull, // screen argb <- rgb, level 0x7f
	0xc0582f611b5c1b31ull, // screen argb <- rgb, level 0x80
	0x5d39f46fa9ce3938ull, // screen argb <- rgb, level 0xff
	0x05bdfe2f2c6a5ff0ull, // screen argb <- rgb, level 0x100
	0x6aee09302afdac21ull, // screen argb <- rgb, level 0x101
	0xd6bf4ff90e1bb7d5ull, // screen argb <- rgb, level 0x1c0
	0xf64997e12ccf2b24ull, // screen argb <- argb, level 0x1
	0x8ed0351a4b7e60d9ull, // screen argb <- argb, level 0x40
	0xf2ae96fdfe514a7dull, // screen argb <- argb, level 0x7f
	0xc0582f611b5c1b31ull, // screen argb <- argb, level 0x80
	0x5d39f46fa9ce3938ull, // screen argb <- argb, level 0xff
	0x05bdfe2f2c6a5ff0ull, // screen argb <- argb, level 0x100
	0x6aee09302afdac21ull, // screen argb <- argb, level 0x101
	0xd6bf4ff90e1bb7d5ull, // screen argb <- argb, level 0x1c0
	0x6a9e474db9c5cb49ull, // dim rgb <- rgb, level 0x1
	0x1f4a5a2b5e3e72b0ull, // dim rgb <- rgb, level 0x2
	0x0dc525e612094bd7ull, // dim rgb <- rgb, level 0x3f
	0xbcb742b506716035ull, // dim rgb <- rgb, level 0x80
	0x2d065c9b2d7f5b8aull, // dim rgb <- rgb, level 0x81
	0xef12fee91f0e50f9ull, // dim rgb <- rgb, level 0xfe
	0xf718f22467ca8833ull, // dim rgb <- rgb, level 0xff
	0x05afb8143840bfdcull, // dim rgb <- argb, level 0x1
	0x3500a3f6ee99e0d5ull, // dim rgb <- argb, level 0x2
	0xaefe6cc22f29c478ull, // dim rgb <- argb, level 0x3f
	0xee43d746306f3601ull, // dim rgb <- argb, level 0x80
	0xc7bd6354a77336adull, // dim rgb <- argb, level 0x81
	0x857faa6c5199cfb9ull, // dim rgb <- argb, level 0xfe
	0xd64aacc3b9f53b25ull, // dim rgb <- argb, level 0xff
	0xf64997e12ccf2b24ull, // dim argb <- rgb, level 0x1
	0x8ed0351a4b7e60d9ull, // dim argb <- rgb, level 0x2
	0xf2ae96fdfe514a7dull, // dim argb <- rgb, level 0x3f
	0xc0582f611b5c1b31ull, // dim argb <- rgb, level 0x80
	0x5d39f46fa9ce3938ull, // dim argb <- rgb, level 0x81
	0x05bdfe2f2c6a5ff0ull, // dim argb <- rgb, level 0xfe
	0x6aee09302afdac21ull, // dim argb <- rgb, level 0xff
	0x2dea78f222b6da41ull, // dim argb <- argb, level 0x1
	0xc9261718dbdde892ull, // dim argb <- argb, level 0x2
	0xb43f5c9f23726457ull, // dim argb <- argb, level 0x3f
	0x0505af006e27decbull, // dim argb <- argb, level 0x80
	0xcc3cca34fbdf4f2cull, // dim argb <- argb, level 0x81
	0x18108b814dd3047dull, // dim argb <- argb, level 0xfe
	0xc6b407efaca646f2ull, // dim argb <- argb, level 0xff
	0x385326f4264f20d0ull, // xform <- rgb, nearest, turn 0, level 0x0
	0xe058c9e9c02c0f29ull, // xform <- rgb, nearest, turn 0, level 0x1
	0x890f8b9d42f95283ull, // xform <- rgb, nearest, turn 0, level 0x80
	0x7788b193c75a3d99ull, // xform <- rgb, nearest, turn 0, level 0xff
	0x680c9c807358ee0bull, // xform <- rgb, nearest, turn 1, level 0x0
	0xa759e9c119758b56ull, // xform <- rgb, nearest, turn 1, level 0x1
	0x1199ba7a629e1599ull, // xform <- rgb, nearest, turn 1, level 0x80
	0x49569a914eb5d029ull, // xform <- rgb, nearest, turn 1, level 0xff
	0xc8b60ae94018d4daull, // xform <- rgb, nearest, turn 2, level 0x0
	0x834b0cb6017833b3ull, // xform <- rgb, nearest, turn 2, level 0x1
	0x5f1f0b6f8a48d956ull, // xform <- rgb, nearest, turn 2, level 0x80
	0xbb327709b62b39d5ull, // xform <- rgb, nearest, turn 2, level 0xff
	0xe44a1b99e8812a0dull, // xform <- rgb, nearest, turn 3, level 0x0
	0x12121575f56faaaeull, // xform <- rgb, nearest, turn 3, level 0x1
	0x451e6336a3dfd977ull, // xform <- rgb, nearest, turn 3, level 0x80
	0xc08dc7fda703293aull, // xform <- rgb, nearest, turn 3, level 0xff
	0xda6931a0cff2422bull, // xform <- rgb, nearest, turn 4, level 0x0
	0x229da1454bd2a21full, // xform <- rgb, nearest, turn 4, level 0x1
	0x22085451bade3565ull, // xform <- rgb, nearest, turn 4, level 0x80
	0x5bb5a6a3d7d6a234ull, // xform <- rgb, nearest, turn 4, level 0xff
	0x02180a305f876249ull, // xform <- rgb, nearest, turn 5, level 0x0
	0x009d6503723b2177ull, // xform <- rgb, nearest, turn 5, level 0x1
	0x1522870a17ca40d3ull, // xform <- rgb, nearest, turn 5, level 0x80
	0x67064068cd92ce19ull, // xform <- rgb, nearest, turn 5, level 0xff
	0x6a5506e83b5d302dull, // xform <- rgb, nearest, turn 6, level 0x0
	0xa87ae8092deda743ull, // xform <- rgb, nearest, turn 6, level 0x1
	0x86fc0afc62877551ull, // xform <- rgb, nearest, turn 6, level 0x80
	0x824c7d55f0ccc5b0ull, // xform <- rgb, nearest, turn 6, level 0xff
	0xf7474f7be635de08ull, // xform <- rgb, nearest, turn 7, level 0x0
	0x7328dc150a33096bull, // xform <- rgb, nearest, turn 7, level 0x1
	0x0f7ca819e899b6c2ull, // xform <- rgb, nearest, turn 7, level 0x80
	0x4eebf13ed8959af8ull, // xform <- rgb, nearest, turn 7, level 0xff
	0x385326f4264f20d0ull, // xform <- rgb, smooth, turn 0, level 0x0
	0xe058c9e9c02c0f29ull, // xform <- rgb, smooth, turn 0, level 0x1
	0x890f8b9d42f95283ull, // xform <- rgb, smooth, turn 0, level 0x80
	0x7788b193c75a3d99ull, // xform <- rgb, smooth, turn 0, level 0xff
	0x945821598e2bd0afull, // xform <- rgb, smooth, turn 1, level 0x0
	0x18e43030b0159ef7ull, // xform <- rgb, smooth, turn 1, level 0x1
	0xae1e742a1223b116ull, // xform <- rgb, smooth, turn 1, level 0x80
	0xc0d1b53d31e683f6ull, // xform <- rgb, smooth, turn 1, level 0xff
	0xb76463664a747aeeull, // xform <- rgb, smooth, turn 2, level 0x0
	0x10fb2d40ae398d61ull, // xform <- rgb, smooth, turn 2, level 0x1
	0xd05c392525d68a64ull, // xform <- rgb, smooth, turn 2, level 0x80
	0xcc84f37ee80a11afull, // xform <- rgb, smooth, turn 2, level 0xff
	0x27c0c47b91786f2bull, // xform <- rgb, smooth, turn 3, level 0x0
	0xcdca613cf071213dull, // xform <- rgb, smooth, turn 3, level 0x1
	0x2f69bcbf87bdce0eull, // xform <- rgb, smooth, turn 3, level 0x80
	0x6e84724b28e951a8ull, // xform <- rgb, smooth, turn 3, level 0xff
	0x6f406aefaaca9637ull, // xform <- rgb, smooth, turn 4, level 0x0
	0xeade2972ee2a1798ull, // xform <- rgb, smooth, turn 4, level 0x1
	0xb0b248f9cf5d8ed6ull, // xform <- rgb, smooth, turn 4, level 0x80
	0x90c5d713cbfd5640ull, // xform <- rgb, smooth, turn 4, level 0xff
	0xba620393a9f64370ull, // xform <- rgb, smooth, turn 5, level 0x0
	0x058c673353246640ull, // xform <- rgb, smooth, turn 5, level 0x1
	0x3a2e2cd9c3535902ull, // xform <- rgb, smooth, turn 5, level 0x80
	0x230e0eafcbe291fdull, // xform <- rgb, smooth, turn 5, level 0xff
	0x8072cf587b07dba0ull, // xform <- rgb, smooth, turn 6, level 0x0
	0x2b8aa0d66034d2a3ull, // xform <- rgb, smooth, turn 6, level 0x1
	0xbdc9f83f3c23dec2ull, // xform <- rgb, smooth, turn 6, level 0x80
	0xd25dd20d4205e01dull, // xform <- rgb, smooth, turn 6, level 0xff
	0x0fd954f20eedb4eaull, // xform <- rgb, smooth, turn 7, level 0x0
	0x21522abfc0732874ull, // xform <- rgb, smooth, turn 7, level 0x1
	0x323f06ef51f16fa4ull, // xform <- rgb, smooth, turn 7, level 0x80
	0xfa601545f513e757ull, // xform <- rgb, smooth, turn 7, level 0xff
	0x40fc32d5d424d01full, // xform <- argb, nearest, turn 0, level 0x0
	0xc2f18139cf29ad8full, // xform <- argb, nearest, turn 0, level 0x1
	0xb5eeb06f3848b144ull, // xform <- argb, nearest, turn 0, level 0x80
	0x1d9012a3928de3edull, // xform <- argb, nearest, turn 0, level 0xff
	0x3f5950b676163f8bull, // xform <- argb, nearest, turn 1, level 0x0
	0xf42b54e7816df8d2ull, // xform <- argb, nearest, turn 1, level 0x1
	0x496f93a221db4d9cull, // xform <- argb, nearest, turn 1, level 0x80
	0x496de27c127c2623ull, // xform <- argb, nearest, turn 1, level 0xff
	0x664239f5c6e2197aull, // xform <- argb, nearest, turn 2, level 0x0
	0x4704ca0dd4c9cffbull, // xform <- argb, nearest, turn 2, level 0x1
	0x1dd3547c6be2770aull, // xform <- argb, nearest, turn 2, level 0x80
	0x4b210c69acec6279ull, // xform <- argb, nearest, turn 2, level 0xff
	0xa5604075506a0346ull, // xform <- argb, nearest, turn 3, level 0x0
	0x67eb6985ed21c739ull, // xform <- argb, nearest, turn 3, level 0x1
	0x555e4765e37ed46dull, // xform <- argb, nearest, turn 3, level 0x80
	0x68c5d5d4696cb133ull, // xform <- argb, nearest, turn 3, level 0xff
	0xde6a1046d4aa56adull, // xform <- argb, nearest, turn 4, level 0x0
	0xa3d4fff7b4f977eeull, // xform <- argb, nearest, turn 4, level 0x1
	0xf233bf1790c5a1c4ull, // xform <- argb, nearest, turn 4, level 0x80
	0x6daedcf9f1f59595ull, // xform <- argb, nearest, turn 4, level 0xff
	0x53c88b0b9359da3dull, // xform <- argb, nearest, turn 5, level 0x0
	0xd7bdd36118b8d5f0ull, // xform <- argb, nearest, turn 5, level 0x1
	0x55ed5706135e6bcfull, // xform <- argb, nearest, turn 5, level 0x80
	0xdb618a92e94fb2bfull, // xform <- argb, nearest, turn 5, level 0xff
	0x6e475144146ab2b4ull, // xform <- argb, nearest, turn 6, level 0x0
	0x8a135da3dfa27647ull, // xform <- argb, nearest, turn 6, level 0x1
	0xb7e1530bf362639cull, // xform <- argb, nearest, turn 6, level 0x80
	0xf64997e12ccf2b24ull, // xform <- argb, nearest, turn 6, level 0xff
	0xe0a13aab66998c7eull, // xform <- argb, nearest, turn 7, level 0x0
	0x540ed1e9adc6cd4eull, // xform <- argb, nearest, turn 7, level 0x1
	0xae5fd9f7caad07fbull, // xform <- argb, nearest, turn 7, level 0x80
	0x8ed0351a4b7e60d9ull, // xform <- argb, nearest, turn 7, level 0xff
	0x40fc32d5d424d01full, // xform <- argb, smooth, turn 0, level 0x0
	0xc2f18139cf29ad8full, // xform <- argb, smooth, turn 0, level 0x1
	0xb5eeb06f3848b144ull, // xform <- argb, smooth, turn 0, level 0x80
	0x1d9012a3928de3edull, // xform <- argb, smooth, turn 0, level 0xff
	0x09225f12558ebc85ull, // xform <- argb, smooth, turn 1, level 0x0
	0x9d1c8217593c3f6eull, // xform <- argb, smooth, turn 1, level 0x1
	0x9a5a57c15f730cdaull, // xform <- argb, smooth, turn 1, level 0x80
	0x496de27c127c2623ull, // xform <- argb, smooth, turn 1, level 0xff
	0x6d2bb4853bb019e2ull, // xform <- argb, smooth, turn 2, level 0x0
	0x6d74fc3db0fa1221ull, // xform <- argb, smooth, turn 2, level 0x1
	0xef72615a284dc92aull, // xform <- argb, smooth, turn 2, level 0x80
	0x4b210c69acec6279ull, // xform <- argb, smooth, turn 2, level 0xff
	0x64b3e2b09cc5a254ull, // xform <- argb, smooth, turn 3, level 0x0
	0x613edd9d2da563e7ull, // xform <- argb, smooth, turn 3, level 0x1
	0x7947ff382ffdfd42ull, // xform <- argb, smooth, turn 3, level 0x80
	0x68c5d5d4696cb133ull, // xform <- argb, smooth, turn 3, level 0xff
	0x6cd83b145412e0adull, // xform <- argb, smooth, turn 4, level 0x0
	0x6313e8cd0a1930b4ull, // xform <- argb, smooth, turn 4, level 0x1
	0x4e74517a9adeef68ull, // xform <- argb, smooth, turn 4, level 0x80
	0x6daedcf9f1f59595ull, // xform <- argb, smooth, turn 4, level 0xff
	0x31bff872f8d81f21ull, // xform <- argb, smooth, turn 5, level 0x0
	0x70648f518548b903ull, // xform <- argb, smooth, turn 5, level 0x1
	0x518267dbf8808dbfull, // xform <- argb, smooth, turn 5, level 0x80
	0xdb618a92e94fb2bfull, // xform <- argb, smooth, turn 5, level 0xff
	0x03113d34c74127f2ull, // xform <- argb, smooth, turn 6, level 0x0
	0xf092b62e46b9ae92ull, // xform <- argb, smooth, turn 6, level 0x1
	0x780149b1c814eb39ull, // xform <- argb, smooth, turn 6, level 0x80
	0xf64997e12ccf2b24ull, // xform <- argb, smooth, turn 6, level 0xff
	0xfba080119782793full, // xform <- argb, smooth, turn 7, level 0x0
	0xc4fa0d68ad95dd12ull, // xform <- argb, smooth, turn 7, level 0x1
	0xe4670d8f7f0e5e37ull, // xform <- argb, smooth, turn 7, level 0x80
	0x8ed0351a4b7e60d9ull, // xform <- argb, smooth, turn 7, level 0xff
	0x385326f4264f20d0ull, // xform copy <- rgb, nearest, turn 0, level 0x0
	0xbb9588bdad825fa0ull, // xform copy <- rgb, nearest, turn 0, level 0x1
	0xf71f1ff126ad08d6ull, // xform copy <- rgb, nearest, turn 0, level 0x80
	0x05984eac5914f9eeull, // xform copy <- rgb, nearest, turn 0, level 0xff
	0x680c9c807358ee0bull, // xform copy <- rgb, nearest, turn 1, level 0x0
	0x8cfac4138cfc040dull, // xform copy <- rgb, nearest, turn 1, level 0x1
	0x8dbe752a334816a8ull, // xform copy <- rgb, nearest, turn 1, level 0x80
	0x376c66448f26881dull, // xform copy <- rgb, nearest, turn 1, level 0xff
	0xc8b60ae94018d4daull, // xform copy <- rgb, nearest, turn 2, level 0x0
	0x47b4f2503caeccdaull, // xform copy <- rgb, nearest, turn 2, level 0x1
	0xdc7e339671dfb9feull, // xform copy <- rgb, nearest, turn 2, level 0x80
	0xc552503438275199ull, // xform copy <- rgb, nearest, turn 2, level 0xff
	0xe44a1b99e8812a0dull, // xform copy <- rgb, nearest, turn 3, level 0x0
	0x1724bbe1f1e822edull, // xform copy <- rgb, nearest, turn 3, level 0x1
	0xb0f7312e0f8867ddull, // xform copy <- rgb, nearest, turn 3, level 0x80
	0xb8ca3914d0e0e1f8ull, // xform copy <- rgb, nearest, turn 3, level 0xff
	0xda6931a0cff2422bull, // xform copy <- rgb, nearest, turn 4, level 0x0
	0xba3c2f2a5951eae2ull, // xform copy <- rgb, nearest, turn 4, level 0x1
	0x3ae9a9fd7076fe63ull, // xform copy <- rgb, nearest, turn 4, level 0x80
	0xae3da72251e4a6a2ull, // xform copy <- rgb, nearest, turn 4, level 0xff
	0x02180a305f876249ull, // xform copy <- rgb, nearest, turn 5, level 0x0
	0x7cd9dad834ebddb2ull, // xform copy <- rgb, nearest, turn 5, level 0x1
	0xfe6c82bbc311aa9aull, // xform copy <- rgb, nearest, turn 5, level 0x80
	0x025f06df48961682ull, // xform copy <- rgb, nearest, turn 5, level 0xff
	0x6a5506e83b5d302dull, // xform copy <- rgb, nearest, turn 6, level 0x0
	0x4abb2a8dbd2b79a1ull, // xform copy <- rgb, nearest, turn 6, level 0x1
	0x396484ab7cb0a185ull, // xform copy <- rgb, nearest, turn 6, level 0x80
	0xf1ab61efc67c1104ull, // xform copy <- rgb, nearest, turn 6, level 0xff
	0xf7474f7be635de08ull, // xform copy <- rgb, nearest, turn 7, level 0x0
	0x0da603d69966d53eull, // xform copy <- rgb, nearest, turn 7, level 0x1
	0xafa14da38b1d2038ull, // xform copy <- rgb, nearest, turn 7, level 0x80
	0xaf1ec8c60c3df943ull, // xform copy <- rgb, nearest, turn 7, level 0xff
	0x385326f4264f20d0ull, // xform copy <- rgb, smooth, turn 0, level 0x0
	0xbb9588bdad825fa0ull, // xform copy <- rgb, smooth, turn 0, level 0x1
	0xf71f1ff126ad08d6ull, // xform copy <- rgb, smooth, turn 0, level 0x80
	0x05984eac5914f9eeull, // xform copy <- rgb, smooth, turn 0, level 0xff
	0x945821598e2bd0afull, // xform copy <- rgb, smooth, turn 1, level 0x0
	0x67c9715eadbd8cc1ull, // xform copy <- rgb, smooth, turn 1, level 0x1
	0xb66e849e38967fa4ull, // xform copy <- rgb, smooth, turn 1, level 0x80
	0x656e53579332724bull, // xform copy <- rgb, smooth, turn 1, level 0xff
	0xb76463664a747aeeull, // xform copy <- rgb, smooth, turn 2, level 0x0
	0x7b9017b0d949e1ccull, // xform copy <- rgb, smooth, turn 2, level 0x1
	0x8492be9565312ae3ull, // xform copy <- rgb, smooth, turn 2, level 0x80
	0xdcf02590b9ad773eull, // xform copy <- rgb, smooth, turn 2, level 0xff
	0x27c0c47b91786f2bull, // xform copy <- rgb, smooth, turn 3, level 0x0
	0xfd288bf1300d1fe4ull, // xform copy <- rgb, smooth, turn 3, level 0x1
	0x2f7f9bb15fb44df8ull, // xform copy <- rgb, smooth, turn 3, level 0x80
	0xb5ad55bdf49894a0ull, // xform copy <- rgb, smooth, turn 3, level 0xff
	0x6f406aefaaca9637ull, // xform copy <- rgb, smooth, turn 4, level 0x0
	0x7ffd94f465cfc14dull, // xform copy <- rgb, smooth, turn 4, level 0x1
	0x8b29f73b630d9a07ull, // xform copy <- rgb, smooth, turn 4, level 0x80
	0xa699008a8477d4d9ull, // xform copy <- rgb, smooth, turn 4, level 0xff
	0xba620393a9f64370ull, // xform copy <- rgb, smooth, turn 5, level 0x0
	0xe8de820501ead8e5ull, // xform copy <- rgb, smooth, turn 5, level 0x1
	0xe790c7b69db6c114ull, // xform copy <- rgb, smooth, turn 5, level 0x80
	0xedc46d14cbed4b7cull, // xform copy <- rgb, smooth, turn 5, level 0xff
	0x8072cf587b07dba0ull, // xform copy <- rgb, smooth, turn 6, level 0x0
	0x25d4682fc4fc7886ull, // xform copy <- rgb, smooth, turn 6, level 0x1
	0x525e55678b431855ull, // xform copy <- rgb, smooth, turn 6, level 0x80
	0x3e473a08da7d9a65ull, // xform copy <- rgb, smooth, turn 6, level 0xff
	0x0fd954f20eedb4eaull, // xform copy <- rgb, smooth, turn 7, level 0x0
	0xd249d4b80f99cc8bull, // xform copy <- rgb, smooth, turn 7, level 0x1
	0xaff8fd4d7dae89b8ull, // xform copy <- rgb, smooth, turn 7, level 0x80
	0xdad1c145fae4375aull, // xform copy <- rgb, smooth, turn 7, level 0xff
	0x385326f4264f20d0ull, // xform copy <- argb, nearest, turn 0, level 0x0
	0xbb9588bdad825fa0ull, // xform copy <- argb, nearest, turn 0, level 0x1
	0xf71f1ff126ad08d6ull, // xform copy <- argb, nearest, turn 0, level 0x80
	0x05984eac5914f9eeull, // xform copy <- argb, nearest, turn 0, level 0xff
	0x680c9c807358ee0bull, // xform copy <- argb, nearest, turn 1, level 0x0
	0x8cfac4138cfc040dull, // xform copy <- argb, nearest, turn 1, level 0x1
	0x8dbe752a334816a8ull, // xform copy <- argb, nearest, turn 1, level 0x80
	0x376c66448f26881dull, // xform copy <- argb, nearest, turn 1, level 0xff
	0xc8b60ae94018d4daull, // xform copy <- argb, nearest, turn 2, level 0x0
	0x47b4f2503caeccdaull, // xform copy <- argb, nearest, turn 2, level 0x1
	0xdc7e339671dfb9feull, // xform copy <- argb, nearest, turn 2, level 0x80
	0xc552503438275199ull, // xform copy <- argb, nearest, turn 2, level 0xff
	0xe44a1b99e8812a0dull, // xform copy <- argb, nearest, turn 3, level 0x0
	0x1724bbe1f1e822edull, // xform copy <- argb, nearest, turn 3, level 0x1
	0xb0f7312e0f8867ddull, // xform copy <- argb, nearest, turn 3, level 0x80
	0xb8ca3914d0e0e1f8ull, // xform copy <- argb, nearest, turn 3, level 0xff
	0xda6931a0cff2422bull, // xform copy <- argb, nearest, turn 4, level 0x0
	0xba3c2f2a5951eae2ull, // xform copy <- argb, nearest, turn 4, level 0x1
	0x3ae9a9fd7076fe63ull, // xform copy <- argb, nearest, turn 4, level 0x80
	0xae3da72251e4a6a2ull, // xform copy <- argb, nearest, turn 4, level 0xff
	0x02180a305f876249ull, // xform copy <- argb, nearest, turn 5, level 0x0
	0x7cd9dad834ebddb2ull, // xform copy <- argb, nearest, turn 5, level 0x1
	0xfe6c82bbc311aa9aull, // xform copy <- argb, nearest, turn 5, level 0x80
	0x025f06df48961682ull, // xform copy <- argb, nearest, turn 5, level 0xff
	0x6a5506e83b5d302dull, // xform copy <- argb, nearest, turn 6, level 0x0
	0x4abb2a8dbd2b79a1ull, // xform copy <- argb, nearest, turn 6, level 0x1
	0x396484ab7cb0a185ull, // xform copy <- argb, nearest, turn 6, level 0x80
	0xf1ab61efc67c1104ull, // xform copy <- argb, nearest, turn 6, level 0xff
	0xf7474f7be635de08ull, // xform copy <- argb, nearest, turn 7, level 0x0
	0x0da603d69966d53eull, // xform copy <- argb, nearest, turn 7, level 0x1
	0xafa14da38b1d2038ull, // xform copy <- argb, nearest, turn 7, level 0x80
	0xaf1ec8c60c3df943ull, // xform copy <- argb, nearest, turn 7, level 0xff
	0x385326f4264f20d0ull, // xform copy <- argb, smooth, turn 0, level 0x0
	0xbb9588bdad825fa0ull, // xform copy <- argb, smooth, turn 0, level 0x1
	0xf71f1ff126ad08d6ull, // xform copy <- argb, smooth, turn 0, level 0x80
	0x05984eac5914f9eeull, // xform copy <- argb, smooth, turn 0, level 0xff
	0x945821598e2bd0afull, // xform copy <- argb, smooth, turn 1, level 0x0
	0x67c9715eadbd8cc1ull, // xform copy <- argb, smooth, turn 1, level 0x1
	0xb66e849e38967fa4ull, // xform copy <- argb, smooth, turn 1, level 0x80
	0x656e53579332724bull, // xform copy <- argb, smooth, turn 1, level 0xff
	0xb76463664a747aeeull, // xform copy <- argb, smooth, turn 2, level 0x0
	0x7b9017b0d949e1ccull, // xform copy <- argb, smooth, turn 2, level 0x1
	0x8492be9565312ae3ull, // xform copy <- argb, smooth, turn 2, level 0x80
	0xdcf02590b9ad773eull, // xform copy <- argb, smooth, turn 2, level 0xff
	0x27c0c47b91786f2bull, // xform copy <- argb, smooth, turn 3, level 0x0
	0xfd288bf1300d1fe4ull, // xform copy <- argb, smooth, turn 3, level 0x1
	0x2f7f9bb15fb44df8ull, // xform copy <- argb, smooth, turn 3, level 0x80
	0xb5ad55bdf49894a0ull, // xform copy <- argb, smooth, turn 3, level 0xff
	0x6f406aefaaca9637ull, // xform copy <- argb, smooth, turn 4, level 0x0
	0x7ffd94f465cfc14dull, // xform copy <- argb, smooth, turn 4, level 0x1
	0x8b29f73b630d9a07ull, // xform copy <- argb, smooth, turn 4, level 0x80
	0xa699008a8477d4d9ull, // xform copy <- argb, smooth, turn 4, level 0xff
	0xba620393a9f64370ull, // xform copy <- argb, smooth, turn 5, level 0x0
	0xe8de820501ead8e5ull, // xform copy <- argb, smooth, turn 5, level 0x1
	0xe790c7b69db6c114ull, // xform copy <- argb, smooth, turn 5, level 0x80
	0xedc46d14cbed4b7cull, // xform copy <- argb, smooth, turn 5, level 0xff
	0x8072cf587b07dba0ull, // xform copy <- argb, smooth, turn 6, level 0x0
	0x25d4682fc4fc7886ull, // xform copy <- argb, smooth, turn 6, level 0x1
	0x525e55678b431855ull, // xform copy <- argb, smooth, turn 6, level 0x80
	0x3e473a08da7d9a65ull, // xform copy <- argb, smooth, turn 6, level 0xff
	0x0fd954f20eedb4eaull, // xform copy <- argb, smooth, turn 7, level 0x0
	0xd249d4b80f99cc8bull, // xform copy <- argb, smooth, turn 7, level 0x1
	0xaff8fd4d7dae89b8ull, // xform copy <- argb, smooth, turn 7, level 0x80
	0xdad1c145fae4375aull, // xform copy <- argb, smooth, turn 7, level 0xff
