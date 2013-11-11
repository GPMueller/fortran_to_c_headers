#include "grammar.hpp"
#include <boost/spirit/include/phoenix_function.hpp>
#include <boost/spirit/include/qi_no_case.hpp>


namespace f2h
{

template <typename Iterator>
Grammar<Iterator>::Grammar()
  : Grammar::base_type(program)
{
  program_block = (modificator | function | var_decl | other);

  program = *program_block;

  //BOOST_SPIRIT_DEBUG_NODES(
  //    (program_block)
  //    );

}

}
