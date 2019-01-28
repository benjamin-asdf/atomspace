
(use-modules (opencog))
(use-modules (opencog test-runner))
(use-modules (opencog exec))
(use-modules (opencog python))

(opencog-test-runner)

(define tname "opencog-arity-test")
(test-begin tname)

; Define a python func returning a TV
(python-eval "
from opencog.atomspace import AtomSpace, TruthValue
from opencog.atomspace import types
def foo(atom_a, atom_b):
    asp = AtomSpace()
    TV = TruthValue(0.2, 0.69)
    asp.add_node(types.ConceptNode, 'Apple', TV)
    asp.add_link(types.InheritanceLink, atom_a, atom_b, TV)
    TruthValue(0.42, 0.24)
")

; Call the python func defined above.
(define returned-tv
	(cog-evaluate!
		(Evaluation
			(GroundedPredicate "py:foo")
			(List (Concept "fruit") (Concept "banana")))))

; Make sure that Apple was created.
(test-assert "Apple atom was created"
	(not (eq? #f (cog-node 'ConceptNode "Apple"))))

; Make sure the scheme version of Apple has the same TV on it that
; the python code placed on it.
(test-assert "TV on Apple is wrong"
	(< (abs (- 0.2 (cog-mean (Concept "Apple")))) 0.00001))

(test-assert "returned TV is wrong"
	(< (abs (- 0.42 (cog-tv-mean returned-tv))) 0.00001))

(define (catch-wrong-args thunk)
	(catch #t
		thunk
		(lambda (key . parameters)
			(format (current-error-port)
				"Expected to catch this Python exception: '~a: ~a\n"
				key parameters)
			"woo-hooo!!")))

(test-assert "Threw exception even when given the right number of arguments"
	(eq? (SimpleTruthValue 0.42 0.24)
		(catch-wrong-args (lambda ()
			(cog-evaluate!
				(Evaluation
					(GroundedPredicate "py:foo")
					(List (Concept "fruit") (Concept "banana"))))))))

(test-assert "Failed to throw when given too few arguments"
	(string=? "woo-hooo!!"
		(catch-wrong-args (lambda ()
			(cog-evaluate!
				(Evaluation
					(GroundedPredicate "py:foo")
					(List (Concept "fruit"))))))))

(test-assert "Failed to throw when given too many arguments"
	(string=? "woo-hooo!!"
		(catch-wrong-args (lambda ()
			(cog-evaluate!
				(Evaluation
					(GroundedPredicate "py:foo")
					(List
						(Concept "fruit-stuff")
						(Concept "banana")
						(Concept "orange"))))))))

(test-end tname)
