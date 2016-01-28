#!/usr/bin/env impala-python
# Copyright (c) 2015 Cloudera, Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from time import sleep, localtime, strftime
from tests.comparison.query_profile import DefaultProfile, ImpalaNestedTypesProfile
from schedule_item import ScheduleItem
from fabric.api import sudo, settings
from threading import Thread
import os
import pickle
import time
import logging

PATH_TO_SCHEDULE = '/tmp/query_gen/schedule'
PATH_TO_REPORTS = '/tmp/query_gen/reports'
PATH_TO_FINISHED_JOBS = '/tmp/query_gen/completed_jobs'
PATH_TO_LOG = '/tmp/query_gen/log'
RUN_TIME_LIMIT = 12 * 3600
GENERATION_FREQUENCY = RUN_TIME_LIMIT
MAX_CONCURRENCY = 2
DEFAULT_RUN_NAME = 'AUTO_RUN'
SLEEP_LENGTH = 3

NESTED_TYPES_MODE = False
DELETE_SCHEDULE_ITEMS_ON_STARTUP = True
SHOULD_BUILD_IMPALA = True
SHOULD_PULL_DOCKER_IMAGE = False
DATABASE_NAME = 'functional'
POSTGRES_DATABASE_NAME = 'functional'

LOG = logging.getLogger('Controller')

class Controller(object):
  '''This class controls the query generator. Generates new schedule_items regularly and
  places them into the schedule directory. Schedule_items can also be generated by other
  means (for example front_end.py), so it checks schedule directory regularly and starts
  running new jobs. It seemed easier and more convenient to implement the scheduling
  mechanism this way, rather than use Jenkins.

  This is indended to be running on machine dedicated to be running the query generator.
  TARGET_HOST environment variable should be set to the address of the host that will be
  running Impala. The target machine should have Docker installed and configured. Each job
  will be run in a separate Docker container. The Docker Image can be specified by setting
  the DOCKER_IMAGE_NAME environment variable. The Image needs have Postgres installed and
  appropriate data loaded.

  Attributes:
    schedule_items: Keeps track of active job threads. This maps job id to the thread that
      running it.
    time_last_generated: Stores the time when a schedule was last generated automatically.
      Used to control the rate at which new schedule_items are generated.
  '''

  def __init__(self):
    self.check_env_vars()
    self.make_local_dirs()

    self.schedule_items = {}
    self.time_last_generated = 0

  def make_local_dirs(self):
    '''Create directories for schedule, log and results.
    '''
    if not os.path.exists(PATH_TO_SCHEDULE):
      os.makedirs(PATH_TO_SCHEDULE)
    if DELETE_SCHEDULE_ITEMS_ON_STARTUP:
      for job_id in os.listdir(PATH_TO_SCHEDULE):
        os.remove(os.path.join(PATH_TO_SCHEDULE, job_id))
    if not os.path.exists(PATH_TO_FINISHED_JOBS):
      os.makedirs(PATH_TO_FINISHED_JOBS)
    if not os.path.exists(PATH_TO_REPORTS):
      os.makedirs(PATH_TO_REPORTS)
    try:
      os.remove(PATH_TO_LOG)
    except OSError:
      # Log file could not be removed most likely because it does not exist, so this
      # exception can be ignored.
      pass

  def check_env_vars(self):
    '''Check if all necessary enivornment variables have been set.'''
    if 'DOCKER_PASSWORD' not in os.environ:
      exit('DOCKER_PASSWORD environment variable not set')
    if 'TARGET_HOST' not in os.environ:
      exit('TARGET_HOST environment variable not set')
    if 'TARGET_HOST_USERNAME' not in os.environ:
      exit('TARGET_HOST_USERNAME environment variable not set')
    if 'DOCKER_IMAGE_NAME' not in os.environ:
      print 'DOCKER_IMAGE_NAME environment variable not set'

  def start_new_jobs(self):
    '''Check the schedule directory for new items. If a new item is present, start a new
    job (if maximum concurrency level has not been reached). Each job gets it's own
    thread.
    '''
    from job import Job

    finished_jobs = set(os.listdir(PATH_TO_FINISHED_JOBS))
    for job_id in os.listdir(PATH_TO_SCHEDULE):
      # If schedule item is not already running, start running it
      if job_id not in self.schedule_items and job_id not in finished_jobs and len(
          self.schedule_items) < MAX_CONCURRENCY:
        with open(os.path.join(PATH_TO_SCHEDULE, job_id), 'r') as f:
          schedule_item = pickle.load(f)
        job = schedule_item.generate_job()
        thread = Thread(target = job.start, name = job_id)
        thread.daemon = True
        LOG.info('Created Job Thread: {0}'.format(job_id))
        self.schedule_items[job_id] = thread
        thread.start()
        sleep(SLEEP_LENGTH)

  def generate_schedule_item(self):
    '''Generate a default schedule_item. This method should normally be called every few
    hours.
    '''
    if self.should_generate_new_item():
      profile = ImpalaNestedTypesProfile() if NESTED_TYPES_MODE else DefaultProfile()
      schedule_item = ScheduleItem(
          run_name = '{0}-{1}'.format(strftime(
              "%Y-%b-%d-%H:%M:%S", localtime()), DEFAULT_RUN_NAME),
          query_profile=profile,
          time_limit_sec=RUN_TIME_LIMIT)
      schedule_item.save_pickle()
      self.time_last_generated = time.time()
      LOG.info('Generated Schedule Item')
    sleep(2)

  def should_generate_new_item(self):
    '''Returns true if a new item should be generated.
    '''
    return time.time() - self.time_last_generated > GENERATION_FREQUENCY

  def run(self):
    '''Main method for the Controller class. Keeps track of how many threads are alive,
    generates new schedule items and starts running new jobs.
    '''
    while True:
      self.schedule_items = dict([(run_id, thread) for run_id, thread
        in self.schedule_items.items() if self.schedule_items[run_id].isAlive()])
      LOG.info('Number of Active Threads: {0}'.format(len(self.schedule_items)))
      self.generate_schedule_item()
      self.start_new_jobs()
      sleep(SLEEP_LENGTH)

if __name__ == '__main__':
  controller = Controller()
  logging.basicConfig(level=logging.DEBUG,
      filename=PATH_TO_LOG,
      format='%(asctime)s %(threadName)s:%(module)s[%(lineno)s]:%(message)s',
      datefmt='%H:%M:%S')
  controller.run()
