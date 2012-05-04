#!/usr/bin/python

import os
import subprocess
import sys

# The names of each of the SLEEP_TYPES
SLEEP_TYPE_NAMES = [ "none", "select", "poll", "usleep", "sched_yield", "pthread_cond", "nanosleep" ]

def make_gnuplot(commands, data, make_svg):
    # Create the arguments to call gnuplot with
    args = ["gnuplot", "-persist", "-e", ";".join(commands)]
    # Start the process so we can write data to it
    program = subprocess.Popen(args, stdin=subprocess.PIPE)
    # Write all of the data
    for d in data:
        for line in d:
            program.stdin.write(line + os.linesep)
        program.stdin.write('e' + os.linesep)

def plot_results(test_name, make_svg):
    # Setup the properties of the plot
    commands = [
        "set key top left",
        "set title '%s'" % test_name,
        "set xlabel 'Number of Threads'",
        "set ylabel 'Time per Iteration (us)'"
        ]

    # Set it to create a .svg, if requested
    if make_svg:
        commands += ["set terminal svg", "set output '%s.svg'" % test_name]

    # Read the data for the given test
    data = []
    while True:
        # Read the data file
        try:
            f = open('%s/results_%d.txt' % (test_name, len(data)), 'r')
        except IOError:
            break
        d = []
        # Process each of the lines
        for i, line in enumerate(f):
            v = line.split()
            # And add the number of threads ax
            num_threads = i + 1
            avg_time = float(v[5])
            stddev_time = float(v[11])
            d.append('%d %f %f' % (num_threads, avg_time, stddev_time))
        data.append(d)

    # Get the number of sleep types for this test
    num_sleep_types = len(data)

    # Create the individual plot commands
    plot_commands = ["'-' title '%s' w errorlines" % SLEEP_TYPE_NAMES[i] for i in range(num_sleep_types)]
    # And add them as a single plot command
    commands.append('plot ' + ','.join(plot_commands))

    # Plot the results with gnuplot
    make_gnuplot(commands, data, make_svg)

if __name__ == '__main__':
    # Make sure the right arguments were given
    if len(sys.argv) < 2:
        print "Usage: %s <test_name> [make_svg]" % sys.argv[0]
        sys.exit(-1)

    # Check if it should create a .svg
    make_svg = len(sys.argv) >= 3 and sys.argv[2].lower() in ('y', 'yes', 'svg')

    # Plot the results of the given test
    plot_results(sys.argv[1], make_svg)
