from Numeric import *
import Nio
import time


def getUserName():
    try:
	import os, pwd, string
    except ImportError:
	return 'unknown user'
    pwd_entry = pwd.getpwuid(os.getuid())
    name = string.strip(string.splitfields(pwd_entry[4], ',')[0])
    if name == '':
	name = pwd_entry[0]
    return name

#
# Creating a file
#
file = Nio.open_file('test.nc', 'w', 'Created ' + time.ctime(time.time())
		  + ' by ' + getUserName())

file.title = "Just some useless junk"
#if "series" in file.__dict__:
#	del file.__dict__['series']
file.series = [ 1, 2, 3, 4, 5,6 ]
file.version = 45
#del file.version

file.create_dimension('xyz', 3)
file.create_dimension('n', 20)
file.create_dimension('t', None) # unlimited dimension

foo = file.create_variable('foo', Int, ('n', 'xyz'))
foo[:,:] = 0.
foo[0,:] = [42., 42.1, 42.2]
foo[:,1] = 1.
foo.units = "arbitrary"
print foo[0]
print foo.dimensions

bar = file.create_variable('bar', Int, ('t', 'n'))
for i in range(10):
    bar[i] = i
print bar.shape

print file.dimensions
print file.variables

file.close()

#
# Reading a file
#
file = Nio.open_file('test.nc', 'r')

print file.dimensions
print file.variables

foo = file.variables['foo']
foo_array = foo[:]
foo_units = foo.units
print foo[0]

file.close()