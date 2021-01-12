from setuptools import setup, Extension

c_src = ['import7z.c',
		 'lzma/7zAlloc.c',
		 'lzma/7zArcIn.c',
		 'lzma/7zBuf.c',
		 'lzma/7zCrc.c',
		 'lzma/7zCrcOpt.c',
		 'lzma/7zDec.c',
		 'lzma/7zFile.c',
		 'lzma/7zStream.c',
		 'lzma/Bcj2.c',
		 'lzma/Bra.c',
		 'lzma/Bra86.c',
		 'lzma/BraIA64.c',
		 'lzma/CpuArch.c',
		 'lzma/Delta.c',
		 'lzma/Lzma2Dec.c',
		 'lzma/LzmaDec.c',
		 'lzma/Ppmd7.c',
		 'lzma/Ppmd7Dec.c']


setup(name='import7z',
      version='0.1.0',
      author='Jeonghun Lee',
      url='https://github.com/leejeonghun/import7z',
      ext_modules=[Extension('import7z', sources=c_src)],
      license='PSF',
      test_suite='test',
      platforms=['any'])
