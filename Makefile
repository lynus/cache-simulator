SOURCE= main.cc rule.hpp
boost_lib_path= ~/boost/lib
boost_inc_path= ~/boost/include

OBJECTS=$(subst .cc,.o,$(SOURCE))

CXXFLAGS=\
	--std=c++11 \
	-g \
	-I $(boost_inc_path) \
	-L $(boost_lib_path) 
.SUFFIXES: .o .cc

%.o: %.cc
	$(CXX) -c  $(CXXFLAGS) -o $@ $<

cache_sim: $(OBJECTS)
	g++ $+ $(CXXFLAGS) -lboost_serialization -lpthread -static  -o $@ 
clean:
	rm cache_sim *.o
