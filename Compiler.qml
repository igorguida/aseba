import QtQuick 2.0

/*
	The execution model is specified by Threads, which execute concurrently.
	Each thread executes a state machine, so it has a start state.
	A thread can transiton from its current state to an other state.

	Each transition has a set of conditions (events),
	which must all become true for the transition to happen.
	If all the conditions of a possible transition are already met when the corresponding state is entered,
	the transition is not yet taken. We wait for any of these conditions to be false
	and then for all these conditions to be true again (raising edge trigger).
	This rule does not hold for a thread's initial state.

	Each transition has a set of actions which are executed when the transition is taken.

	The generated code has an AESL event handler for each thread,
	which tests all the possible transitions for the thread's current state.
	When a transition is taken, an AESL custom event is triggered so the UI can show it.
*/
Item {
	id: compiler

	/*
		// each thread's start state, maximum 2^^16 elements
		input startStates: State[]

		State: {
			// outgoing transitions, maximum 16 elements per state
			transitions: Transition[]
			// set during program execution when this state is active
			active: boolean
		}
		Transition: {
			// no maximum
			events: Event[]
			// no maximum
			actions: Action[]
			// destination state
			next: State
			// called during program execution when the transition is taken
			trigger(): void
		}
		Event: {
			compile(): EventCode
		}
		EventCode: {
			// the event name when the condition can change
			event: string
			// the AESL boolean expression that evaluates this event's condition
			condition: string
		}
		Action: {
			// returns the AESL statements that execute the action
			compile(): string
		}
	  */
	property var ast

	// output
	property var output:
		({
			 // error message, if an error happened during compilation
			 error: "",
			 // the AESL custom event names and arg sizes
			 events: {},
			 // the AESL program text
			 script: "",
		 })

	// internal state during program execution
	property var internal

	onAstChanged: {
		timer.start();
	}

	Timer {
		id: timer
		interval: 0
		onTriggered: {
			try {
				var result = compile(ast);
				output = result.output;
				internal = result.internal;
			} catch(error) {
				if (typeof(error) === "string") {
					output = {
						error: error,
						events: {},
						script: "",
					};
					internal = {
						states: [],
						transitions: [],
					};
				} else {
					throw error;
				}
			}
		}

		function compile(startStates) {
			function filledArray(length, value) {
				var array = new Array(length);
				for (var i = 0; i < length; ++i) {
					array[i] = value;
				}
				return array;
			}

			// a unique token for each compilation pass
			var version = {};
			var transitionDatas = [];
			var stateDatas = [];
			var threadDatas = [];
			// for each AESL event name, the threadDatas that could have a transition
			var eventThreads = {};

			function visitTransition(thread, transition) {
				var transitionData = transition.compilationData;
				if (transitionData === undefined || transitionData.version !== version) {
					var index = transitionDatas.length;
					transitionData = {
						version: version,
						// this transition's global index
						index: index,
						// the input object
						transition: transition,
						// AESL events where this condition can change
						events: [],
						// AESL expression that must become true for the transition to trigger
						condition: "(0 == 0)",
						// AESL statements that will be executed when the transition is triggered
						actions: "",
						// the state that becomes active when the transition is triggered
						next: {},
					};
					transitionDatas.push(transitionData);
					transition.compilationData = transitionData;

					transition.events.forEach(function (event) {
						var compiled = event.compile();
						if (transitionData.events.indexOf(compiled.event) === -1) {
							// add the AESL event to the list of events
							// where the transition must be tested again
							transitionData.events.push(compiled.event);
						}
						// build a conjunction with all the conditions for this transition
						transitionData.condition += " and (" + compiled.condition + ")";
					});

					transition.actions.forEach(function (action) {
						var compiled = action.compile();
						// for each action, add the AESL statements
						transitionData.actions += compiled.action + "\n";
					});

					transitionData.next = visitState(thread, transition.next);
				}
				return transitionData;
			}

			function visitState(thread, state) {
				var stateData = state.compilationData;
				if (stateData === undefined || stateData.version !== version) {
					var index = stateDatas.length;
					stateData = {
						version: version,
						// this state's global index
						index: index,
						// the input object
						state: state,
						// the state's thread
						thread: thread,
						// for each AESL event, the transitions that can trigger
						events: {},
						// all the transitions from this state
						transitions: [],
					};
					stateDatas.push(stateData);
					state.compilationData = stateData;

					var transitions = state.transitions;
					for (var i = 0; i < transitions.length; ++i) {
						var transition = transitions[i];
						var transitionData = visitTransition(thread, transition);
						stateData.transitions.push(transitionData);

						// for each transition, collect the AESL events where they can trigger
						transitionData.events.forEach(function(event) {
							var events = stateData.events[event];
							if (events === undefined) {
								events = [];
								stateData.events[event] = events;
							}
							events.push(transitionData);
						});
					}

					Object.keys(stateData.events).forEach(function(event) {
						// add each AESL event to the thread's list of events
						var events = thread.events[event];
						if (events === undefined) {
							events = [];
							thread.events[event] = events;
						}
						events.push(stateData);
					});
				}
				return stateData;
			}

			function visitThread(startState) {
				var index = threadDatas.length;
				var threadData = {
					version: version,
					// this thread's global index
					index: index,
					// the input object
					startState: {},
					// for each AESL event, the states that have a transition that can trigger
					events: {},
				};
				threadDatas.push(threadData);

				threadData.startState = visitState(threadData, startState);

				Object.keys(threadData.events).forEach(function(event) {
					// add each AESL event to the global list of events
					var events = eventThreads[event];
					if (events === undefined) {
						events = [];
						eventThreads[event] = events;
					}
					events.push(threadData);
				});
				return threadData;
			}

			for (var i = 0; i < startStates.length; ++i) {
				var startState = startStates[i];
				visitThread(startState);
			}

			// AESL custom events
			var events = {
				// "transition" AESL event, triggered when a transition is triggered
				// the only argument is the transition index
				"transition": 1,
			};

			var script = "";
			script = Object.keys(eventThreads).reduce(function(script, eventName, eventIndex) {
				// a constant for each AESL event that is used in the program
				script += "const event_" + eventName + " = " + eventIndex + "\n";
				return script;
			}, script);
			// the current event, can be used inside conditions like clap or tap
			script += "var currentEvent = -1" + "\n";
			script += "var currentThread = -1" + "\n";
			// for each thread, its currently active state
			script += "var threadStates[" + threadDatas.length + "] = [" + threadDatas.map(function(thread) { return thread.startState.index; }).join(",") + "]" + "\n";
			// for each thread, how many timer ticks have elapsed since we entered its currently active state
			script += "var threadStateAges[" + threadDatas.length + "] = [" + threadDatas.map(function() { return "0"; }).join(",") + "]" + "\n";
			// for each thread, a 16 bits vector indicating which transitions currently have all conditions true
			script += "var threadTransitions[" + threadDatas.length + "] = [" + threadDatas.map(function() { return "0"; }).join(",") + "]" + "\n";
			// temp variables when retesting a transition's conditions
			script += "var transitionsOld = -1" + "\n";
			script += "var transitionsNew = -1" + "\n";
			// setup for the timer event block
			script += "timer.period[0] = 10" + "\n";
			// setup for the microphone event blocks
			script += "mic.threshold = 250" + "\n";
			script = transitionDatas.reduce(function(script, transition) {
				script += "\n";
				// for each transition, this procedure gets called when the transition is triggered
				script += "sub transition" + transition.index + "Trigger" + "\n";
				script += "currentEvent = -1" + "\n";
				// inform the event bus that the transition is triggered
				script += "emit transition [" + transition.index + "]" + "\n";
				// execute the transition's actions
				script += transition.actions + "\n";
				// call the procedure that will initialise the target state
				script += "callsub state" + transition.next.index + "Enter\n";
				return script;
			}, script);
			script = stateDatas.reduce(function(script, state) {
				script += "\n";
				// for each state, this procedure gets called to test all its transition's conditions.
				script += "sub state" + state.index + "Test" + "\n";
				script += "transitionsNew = 0" + "\n";
				script = state.transitions.reduce(function (script, transitionData, transitionIndex) {
					var transitionsMask = filledArray(16, "0");
					transitionsMask[transitionIndex] = "1";
					// for each transition, test all its conditions
					script += "if " + transitionData.condition + " then" + "\n";
					// if all the conditions for this transition are true, set a bit in transitionsNew
					script += "transitionsNew |= 0b" + transitionsMask.join("") + "\n";
					script += "end" + "\n";
					return script;
				}, script);

				// this procedure gets called when the state is entered from any transition
				script += "sub state" + state.index + "Enter" + "\n";
				script = state.transitions.reduce(function (script, transition) {
					if (transition.events.length === 0) {
						// unconditional transitions exit the state immediately
						script += "callsub transition" + transition.index + "Trigger" + "\n";
						script += "return" + "\n";
					}
					return script;
				}, script);
				// set the thread's current state
				script += "threadStates[currentThread] = " + state.index + "\n";
				// reset the thread's current state's age
				script += "threadStateAges[currentThread] = 0" + "\n";
				// test all the state's transitions
				script += "callsub state" + state.index + "Test\n";
				// store the result of the previous test
				script += "threadTransitions[currentThread] = transitionsNew" + "\n";

				return script;
			}, script);
			script = Object.keys(eventThreads).reduce(function(script, event) {
				script += "\n";
				var threads = eventThreads[event];
				script = threads.reduce(function(script, thread) {
					// for each AESL event and thread, this procedure gets called when the event fires
					script += "sub " + event + thread.index + "\n";
					// set the current thread, so the states knows which thread we are executing now
					script += "currentThread = " + thread.index + "\n";

					script = thread.events[event].reduce(function(script, state) {
						// for each state, test if the current thread is in this state
						script += "if threadStates[currentThread] == " + state.index + " then" + "\n";

						script += "transitionsOld = threadTransitions[" + thread.index + "]" + "\n";
						script += "callsub state" + state.index + "Test\n";

						script = state.events[event].reduce(function(script, transition) {
							var transitionIndex = state.transitions.indexOf(transition);
							var transitionsMask = filledArray(16, "0");
							transitionsMask[transitionIndex] = "1";
							// for each transition that is possible in this state and on this event,
							// test whether it has become true right now
							script += "if transitionsOld & 0b" + transitionsMask.join("") + " == 0 and transitionsNew & 0b" + transitionsMask.join("") + " == 0b" + transitionsMask.join("") + " then" + "\n";
							// run the transition
							script += "callsub transition" + transition.index + "Trigger" + "\n";
							// exit the thread's event handler, don't test the other transitions
							script += "return" + "\n";
							script += "end" + "\n";
							return script;
						}, script);

						// store all this state's transition's test results
						script += "threadTransitions[" + thread.index + "] = transitionsNew" + "\n";

						script += "end" + "\n";

						return script;
					}, script);
					return script;
				}, script);

				// AESL event handlers
				script += "onevent " + event + "\n";
				// set the current event, so event conditions can use this variable
				script += "currentEvent = event_" + event + "\n";
				if (event === "timer0") {
					// for the timer0 event, increase this thread's current state's age
					script += "ages += [" + threads.map(function() { return "1"; }) + "]" + "\n";
				}
				script = threads.reduce(function(script, thread) {
					// for each thread, runs the thread's event handler
					script += "callsub " + event + thread.index + "\n";
					return script;
				}, script);
				return script;
			}, script);

			return {
				output: {
					error: undefined,
					events: events,
					script: script,
				},
				internal: {
					threads: threadDatas,
					states: stateDatas,
					transitions: transitionDatas,
				},
			};
		}
	}

	// called when stopping or starting the execution of the program
	function execReset(playing) {
		internal.states.forEach(function (state) {
			// for each state, set it as inactive
			state.state.active = false;
		});
		if (playing) {
			internal.threads.forEach(function(thread) {
				// for each thread, set its initial state as active
				internal.states[thread.startState.index].active = true;
			});
		}
	}

	// called when the "transition" custom AESL event is triggered
	function execTransition(transitionIndex) {
		// trigger the transition animation
		internal.transitions[transitionIndex].transition.trigger();
	}

}
