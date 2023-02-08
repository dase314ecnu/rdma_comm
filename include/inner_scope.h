#ifndef INNER_SCOPE_H
#define INNER_SCOPE_H

template <typename FuncType>
class InnerScopeExit
{
public:
	InnerScopeExit(const FuncType _func) :func(_func) {}
	~InnerScopeExit(){ if (!dismissed){ func(); } }
private:
	FuncType func;
	bool dismissed = false;
};
template <typename F>
InnerScopeExit<F> MakeScopeExit(F f) {
	return InnerScopeExit<F>(f);
};

#define DO_STRING_JOIN(arg1, arg2) arg1 ## arg2
#define STRING_JOIN(arg1, arg2) DO_STRING_JOIN(arg1, arg2)
#define SCOPEEXIT(code) auto STRING_JOIN(scope_exit_object_, __LINE__) = MakeScopeExit(code);

#endif