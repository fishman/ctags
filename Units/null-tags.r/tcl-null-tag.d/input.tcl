proc {}  {} {return Empty }
proc aaa {} {return Normal}
proc {a} {} {return Braced}

namespace eval ns1 {
	proc {}  {} {return Empty }
	proc bbb {} {return Normal}
	proc {b} {} {return Braced}
}

namespace eval ns2
proc ns2::    {} {return Empty }
proc ns2::ccc {} {return Normal}
proc {ns2::c} {} {return Braced}
