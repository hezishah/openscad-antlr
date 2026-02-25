// Simple test file
cube([10, 20, 30]);
sphere(r=5);

translate([10, 0, 0])
  cylinder(h=20, r=3);

difference() {
  cube(20, center=true);
  sphere(r=12);
}
