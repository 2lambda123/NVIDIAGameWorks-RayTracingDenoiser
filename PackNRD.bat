rd /q /s "_NRD"
mkdir "_NRD"
cd "_NRD"
mkdir "Lib"
mkdir "Lib\Debug"
mkdir "Lib\Release"
mkdir "Integration"
mkdir "Include"
mkdir "Shaders"
copy "..\_Build\vs2017\Bin\Debug\NRD.dll" "Lib\Debug"
copy "..\_Build\vs2017\Lib\Debug\NRD.lib" "Lib\Debug"
copy "..\_Build\vs2017\Bin\Debug\NRD.pdb" "Lib\Debug"
copy "..\_Build\vs2017\Bin\Release\NRD.dll" "Lib\Release"
copy "..\_Build\vs2017\Lib\Release\NRD.lib" "Lib\Release"
copy "..\_Build\vs2017\Bin\Release\NRD.pdb" "Lib\Release"
copy "..\NRD\Include\*" "Include"
copy "..\NRD\Integration\*" "Integration"
copy "..\NRD\Shaders\NRD.hlsl" "Shaders"
