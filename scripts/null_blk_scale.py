#!/usr/bin/env python3

"""
Test blk-mq scalability with the null-blk kernel module.
"""

import argparse
import datetime
import glob
import json
import multiprocessing
import os
import os.path
import re
import statistics
import subprocess
import sys


def run_fio(args, num_jobs):
    subprocess.check_call(['modprobe', '-r', 'null_blk'])
    subprocess.check_call(['modprobe', 'null_blk', 'queue_mode=2',
                           'hw_queue_depth={}'.format(args.queue_depth),
                           'submit_queues={}'.format(args.hw_queues)])
    if args.disable_iostats:
        with open('/sys/block/nullb0/queue/iostats', 'w') as f:
            f.write('0\n')
    name = 'fio{}'.format(num_jobs)
    output = name + '.json'
    fio_cmd = [
        'fio',
        '--output={}'.format(output),
        '--output-format=json',
        '--name={}'.format(name),
        '--filename=/dev/nullb0',
        '--direct=1',
        '--numjobs={}'.format(num_jobs),
        '--cpus_allowed_policy=split',
        '--runtime=10',
        '--time_based',
        '--ioengine={}'.format(args.ioengine),
        '--iodepth={}'.format(args.iodepth),
        '--rw={}'.format(args.rw),
    ]
    subprocess.check_call(fio_cmd, stdout=subprocess.DEVNULL)

    with open(output, 'r') as f:
        fio_output = json.load(f)
    return aggregate_iops(fio_output)


def aggregate_iops(fio_output):
    read_iops = [job['read']['iops'] for job in fio_output['jobs']]
    read_merges = sum(disk_util['read_merges'] for disk_util in fio_output['disk_util'])
    return {
            'num_jobs': len(fio_output['jobs']),
            'total_iops': sum(read_iops),
            'min_iops': min(read_iops),
            'max_iops': max(read_iops),
            'mean_iops': statistics.mean(read_iops),
            'iops_stdev': statistics.stdev(read_iops) if len(read_iops) > 1 else 0.0,
            'merges': read_merges,
    }


def print_header():
    print('JOBS\tTOTAL IOPS\tMIN IOPS\tMAX IOPS\tMEAN IOPS\tIOPS STDEV\tMERGES', file=sys.stderr)
    sys.stderr.flush()


def print_results(iops):
    print('{num_jobs}\t{total_iops}\t{min_iops}\t{max_iops}\t{mean_iops}\t{iops_stdev}\t{merges}'.format(**iops))
    sys.stdout.flush()


def main():
    def positive_int(value):
        n = int(value)
        if n <= 0:
            raise ValueError
        return n
    parser = argparse.ArgumentParser(
        description='test blk-mq scalability with null-blk',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument(
        '--parse', metavar='PATH', type=str, default=argparse.SUPPRESS,
        help='parse saved result directory instead of running; all other options will be ignored')

    script_group = parser.add_argument_group('script options')
    script_group.add_argument(
        '-m', '--min-jobs', type=int, default=1,
        help='minimum number of jobs to run in parallel')
    script_group.add_argument(
        '-M', '--max-jobs', type=int, default=multiprocessing.cpu_count(),
        help='maximum number of jobs to run in parallel')

    null_blk_group = parser.add_argument_group('null-blk parameters')
    null_blk_group.add_argument(
        '-q', '--hw-queues', type=positive_int, default=multiprocessing.cpu_count(),
        help='number of null-blk hardware queues to use')
    null_blk_group.add_argument(
        '-d', '--queue-depth', type=positive_int, default=64,
        help='depth of null-blk hardware queues')
    null_blk_group.add_argument(
        '--disable-iostats', action='store_true', help='disable iostats')

    fio_group = parser.add_argument_group('fio parameters')
    fio_group.add_argument(
        '--ioengine', type=str, default='libaio', help='I/O engine')
    fio_group.add_argument(
        '--iodepth', type=positive_int, default=64, help='I/O depth')
    fio_group.add_argument(
        '--rw', type=str, default='randread', help='I/O pattern')

    args = parser.parse_args()

    if hasattr(args, 'parse'):
        os.chdir(args.parse)
        print_header()
        paths = glob.glob('fio*.json')
        paths.sort(key=lambda path: int(re.search(r'\d+', path).group()))
        for path in paths:
            with open(path, 'r') as f:
                fio_output = json.load(f)
            iops = aggregate_iops(fio_output)
            print_results(iops)
        return

    now = datetime.datetime.now()
    dir = 'null_blk_scale_' + now.replace(microsecond=0).isoformat()
    os.mkdir(dir)
    print(os.path.abspath(dir), file=sys.stderr)
    os.chdir(dir)

    info = {
        'args': vars(args),
        'date': now.isoformat(),
        'kernel_version': os.uname().release,
    }
    with open('info.json', 'w') as f:
        json.dump(info, f, sort_keys=True, indent=4)

    print_header()
    for num_jobs in range(args.min_jobs, args.max_jobs + 1):
        iops = run_fio(args, num_jobs)
        print_results(iops)


if __name__ == '__main__':
    main()
