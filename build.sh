rm -R scad scad_py
antlr scad.g4 -o scad
antlr scad.g4 -o scad_py -Dlanguage=Python3
touch scad_py/__init__.py
pushd $(pwd)
cd scad
javac -classpath $(which antlr| echo $(dirname $(which antlr))/$(dirname $(xargs readlink))/../antlr-4.9.2-complete.jar) scad*.java
#echo "module hezi () \n { \n }" | grun scad prog -gui
grun scad prog -gui < ../DoorStop.scad
popd
python3 scad-parse.py DoorStop.scad
