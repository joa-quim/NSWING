# NSWING
A tsunami maker

# Compile

It depends on the netCDF library. An example compile command with Visual Studio is:

    cl nswing.c -IC:\programs\compa_libs\netcdf_GIT\compileds\VC12_64\include C:\programs\compa_libs\netcdf_GIT\compileds\VC12_64\lib\netcdf.lib /DI_AM_C /DHAVE_NETCDF /nologo /D_CRT_SECURE_NO_WARNINGS /fp:precise /Ox
