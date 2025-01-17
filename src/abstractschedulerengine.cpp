/* 
 * This file is part of GREASY software package
 * Copyright (C) by the BSC-Support Team, see www.bsc.es
 * 
 * GREASY is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 * 
 * GREASY is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GREASY. If not, see <http://www.gnu.org/licenses/>.
 *
*/

#include "abstractschedulerengine.h"
#include <csignal>
#include <cstdlib>
#include <cstring>


AbstractSchedulerEngine::AbstractSchedulerEngine ( const string& filename) : AbstractEngine(filename){
  
    engineType="abstractscheduler";
    
}

void AbstractSchedulerEngine::init() {

  log->record(GreasyLog::devel, "AbstractSchedulerEngine::init", "Entering...");
  
  AbstractEngine::init();
  
  if (usecpubinding) {
    log->record(GreasyLog::info, "Creating " + toString(nworkers) + " CPU binding workers with strides for " + toString(n_node_cpus) + " CPUs." );
  }
  
  // Fill the freeWorkers queue
  // If there is cpu binding it will create strides. This is only desirable if
  // CPU numbering is done sequentially by socket. If sockets are numbered even/odd
  // then this will create the worst possible load splitting. 
  for (int i=0; i<nworkers; i++) {
    if (usecpubinding) {
      worker_id = i*n_node_cpus/nworkers; // will create evenly spaced strides
    } else {
      worker_id = i;
    }
    freeWorkers.push(worker_id);
  }
  
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::init", "Exiting...");
  
}

void AbstractSchedulerEngine::finalize() {

  AbstractEngine::finalize();
  
}

void AbstractSchedulerEngine::writeRestartFile() {

  AbstractEngine::writeRestartFile();
  
}

void AbstractSchedulerEngine::dumpTasks() {

  AbstractEngine::dumpTasks();
  
}

void AbstractSchedulerEngine::runScheduler() {
  
  //Master code
  set<int>::iterator it;
  GreasyTask* task = NULL;

  log->record(GreasyLog::devel, "AbstractSchedulerEngine::runScheduler", "Entering...");
  
  // Dummy check: let's see if there is any worker...
  if (nworkers==0) {
    log->record(GreasyLog::error, "No workers found. Rerun greasy with more resources");    
    return;
  }
  
  globalTimer.start();

  // Initialize the task queue with all the tasks ready to be executed
  for (it=validTasks.begin();it!=validTasks.end(); it++) {
    task = taskMap[*it];
    if (task->isWaiting()) taskQueue.push(task);
    else if (task->isBlocked()) blockedTasks.insert(task);
  }
   
  // Main Scheduling loop
  while (!(taskQueue.empty())||!(blockedTasks.empty())) {
    while (!taskQueue.empty()) {
      if (!freeWorkers.empty()) {
	// There is room to allocate a task...
	task =  taskQueue.front();
	taskQueue.pop();
	allocate(task);
      } else {
	// All workers are busy. We need to wait anyone to finish.
	waitForAnyWorker();
      }
    }
    
    if (!(blockedTasks.empty())) {
      // There are no tasks to be scheduled on the queue, but there are
      // dependencies not fulfilled and tasks already running, so we have
      // to wait for them to finish to release blocks on them.
      waitForAnyWorker();
    }
  }

  // At this point, all tasks are allocated / finished
  // Wait for the last tasks to complete
  while (freeWorkers.size()!=nworkers) {
    waitForAnyWorker();
  }
  
  globalTimer.stop();
  
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::runScheduler", "Exiting...");
  
}


void AbstractSchedulerEngine::updateDependencies(GreasyTask* parent) {
  
  int taskId, state;
  GreasyTask* child;
  list<int>::iterator it;

  log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Entering...");
  
  taskId = parent->getTaskId();
  state = parent->getTaskState();
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Inspecting reverse deps for task " + toString(taskId));
  
  if ( revDepMap.find(taskId) == revDepMap.end() ){
      log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "The task "+ toString(taskId) + " does not have any other dependendant task. No update done.");
      log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Exiting...");
      return;
  }

  for(it=revDepMap[taskId].begin() ; it!=revDepMap[taskId].end();it++ ) {
    child = taskMap[*it];
    if (state == GreasyTask::completed) {
      log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Remove dependency " + toString(taskId) + " from task " + toString(child->getTaskId()));
      child->removeDependency(taskId);
      if (!child->hasDependencies()) { 
	log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Moving task from blocked set to the queue");
	blockedTasks.erase(child);
	taskQueue.push(child);
      } else {
	log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "The task still has dependencies, so leave it blocked");
      }
    }
    else if ((state == GreasyTask::failed)||(state == GreasyTask::cancelled)) {
      log->record(GreasyLog::warning,  "Cancelling task " + toString(child->getTaskId()) + " because of task " + toString(taskId) + " failure");
      log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Parent failed: cancelling task and removing it from blocked");
      child->setTaskState(GreasyTask::cancelled);
      blockedTasks.erase(child);
      updateDependencies(child);
    }
  }
  
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::updateDependencies", "Exiting...");
  
}

void AbstractSchedulerEngine::taskEpilogue(GreasyTask *task) {
  
  int maxRetries=0;

  log->record(GreasyLog::devel, "AbstractSchedulerEngine::taskEpilogue", "Entering...");
  
  if (config->keyExists("MaxRetries")) fromString(maxRetries, config->getValue("MaxRetries"));
      
  if (task->getReturnCode() != 0) {
    log->record(GreasyLog::error,  "Task " + toString(task->getTaskNum()) + " located in line " + toString(task->getTaskId()) + 
		    " failed with exit code " + toString(task->getReturnCode()) + " on node " + 
		    task->getHostname() +". Elapsed: " + GreasyTimer::secsToTime(task->getElapsedTime()));
    // Task failed, let's retry if we need to
    if ((maxRetries > 0) && (task->getRetries() < maxRetries)) {
      log->record(GreasyLog::warning,  "Retry "+ toString(task->getRetries()) + 
		    "/" + toString(maxRetries) + " of task " + toString(task->getTaskId()));
      task->addRetryAttempt();
      allocate(task);
    } else {
      task->setTaskState(GreasyTask::failed);
      updateDependencies(task);
    }
  } else {
    log->record(GreasyLog::info,  "Task " + toString(task->getTaskNum()) + " located in line " + toString(task->getTaskId()) + 
		    " completed successfully on node " + task->getHostname() + ". Elapsed: " + 
		    GreasyTimer::secsToTime(task->getElapsedTime()));
    task->setTaskState(GreasyTask::completed);
    updateDependencies(task);
  }
  
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::taskEpilogue", "Exiting...");
  
}

void AbstractSchedulerEngine::getDefaultNWorkers() {
 
  nworkers = n_slurm_reservation_cpus;
  if ( nworkers>4 ) nworkers/=2;
  
  log->record(GreasyLog::devel, "AbstractSchedulerEngine::getDefaultNWorkers", "Default nworkers: " + toString(nworkers));
  
}
