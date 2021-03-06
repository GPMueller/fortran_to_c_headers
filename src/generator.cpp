#include "generator.h"

#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/variant/apply_visitor.hpp>

#include "version.h"
#include "exception.h"

namespace f2h
{

Generator::Generator(const std::string &out_file_name)
  : out_file_name_(out_file_name)
{
}

void Generator::Generate(ast::Program const& x, const std::string& define_name, bool add_dll_import, const custom_typedefs_t& typedefs, bool omit_comments, bool add_extern)
{
  // process grammar
  (*this)(x);

  // dump to file
  out_.open(out_file_name_.c_str(), std::ios_base::out | std::ios_base::trunc);

  if (!out_.is_open())
  {
    throw UnableToOpenFileForWritingException();
  }

  std::string dll_export = add_dll_import ? "__declspec(dllimport) " : "";
  std::string extern_str = add_extern ? "extern " : "";

  DumpHeaderStart(define_name, omit_comments);

  BOOST_FOREACH (const custom_typedefs_t::value_type& typedef_input, typedefs)
  {
    BOOST_FOREACH (Argument& arg, globals_)
    {
      ResolveTypedef(arg, typedef_input);
    }

    BOOST_FOREACH (Function& function, functions_)
    {
      BOOST_FOREACH (Argument& arg, function.argument_list)
      {
        ResolveTypedef(arg, typedef_input);
      }
    }
  }

  // generate typedefs
  BOOST_FOREACH (const Typedef& t, registered_typedefs_)
  {
    out_ << "typedef " << t.real_type_name << " " << t.alias << ";\n\n";
  }

  // output global variables
  for (std::list<Argument>::const_iterator it = globals_.begin(); it != globals_.end(); ++it)
  {
    if ((*it).has_c_bind) out_ << extern_str << dll_export << (*it).ToCTypeWithName() << ";\n\n";
  }

  // output functions
  for (std::list<Function>::const_iterator it = functions_.begin(); it != functions_.end(); ++it)
  {
    const Function& function = *it;
    if (!function.has_c_bind) continue;
    out_ << extern_str << dll_export << function.return_value.ToCType() << " " << function.GetName() << "(";
    for (std::list<Argument>::const_iterator argument_it = function.argument_list.begin(); argument_it != function.argument_list.end(); ++argument_it)
    {
      if (argument_it != function.argument_list.begin()) out_ << ", ";
      const Argument& argument = *argument_it;
      out_ << argument.ToCTypeWithName();
    }
    out_ << ");\n\n";
  }

  DumpHeaderEnd(define_name);

  out_.close();
}

void Generator::ResolveTypedef(Argument& arg, const custom_typedefs_t::value_type& typedef_input)
{
  if (!boost::iequals(arg.CName(), typedef_input.first)) return;

  std::list<Typedef>::iterator it = std::find(registered_typedefs_.begin(), registered_typedefs_.end(), arg.CName());

  if (it == registered_typedefs_.end())
  {
    it = RegisterTypedef(Typedef(arg.CName(), arg.ToCType(), typedef_input.second));
  }
  else if ((*it).real_type_name != arg.ToCType())
  {
    throw AmbiguousTypedefException();
  }

  arg.type = (*it).alias;
}

bool Generator::operator()(ast::Identifier const& x)
{
  return true;
}
  
bool Generator::operator()(ast::Other const& x)
{
  return true;
}
  
bool Generator::operator()(ast::Function const& x)
{
  Function new_function;
  new_function.name = x.function_name.name;
  new_function.has_c_bind = x.bind_name;
  new_function.bind_name = x.bind_name ? ExtractBindName(*x.bind_name) : "";
  for (std::list<ast::Identifier>::const_iterator it = x.argument_list.begin(); it != x.argument_list.end(); ++it)
  {
    Argument arg;
    arg.type = "void";
    arg.name = (*it).name;
    new_function.argument_list.push_back(arg);
  }

  // set return value
  new_function.return_value.type = (x.type_spec_prefix) ? boost::apply_visitor(TypeSpecToCType(), *x.type_spec_prefix) : "void";
  new_function.return_value.name = (x.result) ? (*x.result).name : x.function_name.name;
  new_function.return_value.pointer = false;
  new_function.return_value.constant = false;

  functions_.push_back(new_function);
  return true;
}
  
bool Generator::operator()(ast::VariableDeclaration const& x)
{
  if (!boost::apply_visitor(*this, x))
  {
    return false;
  }
  return true;
}
  
bool Generator::operator()(ast::VariableDeclarationAttribute const& x)
{
  if (functions_.empty()) return true;
  Function& current_function = functions_.back();
  for (std::list<ast::Identifier>::const_iterator it = x.variables.begin(); it != x.variables.end(); ++it)
  {
    current_function.SetArgumentAttribute((*it).name, x.keyword);
  }
  return true;
}

bool Generator::operator()(ast::VariableDeclarationSimple const& x)
{
  for (std::list<ast::Identifier>::const_iterator it = x.variables.begin(); it != x.variables.end(); ++it)
  {
    if (!functions_.empty())
    {
      // set argument type for function argument
      functions_.back().SetArgumentType((*it).name, x.type_spec);
    }
    else
    {
      // no functions yet, add global variable
      Argument& arg = this->GetOrAddGlobalArgument((*it).name);
      arg.SetArgumentType(x.type_spec);
    }
  }

  return true;
}

bool Generator::operator()(ast::VariableDeclarationExtended const& x)
{
  for (std::list<ast::Identifier>::const_iterator it = x.variables.begin(); it != x.variables.end(); ++it)
  {
    if (!functions_.empty())
    {
      // set argument type and attributes on function argument
      Function& current_function = functions_.back();
      current_function.SetArgumentType((*it).name, x.type_spec);
      for (std::list<std::string>::const_iterator attribute_it = x.attributes.begin(); attribute_it != x.attributes.end(); ++attribute_it)
      {
        current_function.SetArgumentAttribute((*it).name, *attribute_it);
      }
    }
    else
    {
      // no functions yet, add global variable
      Argument& arg = this->GetOrAddGlobalArgument((*it).name);
      arg.SetArgumentType(x.type_spec);
      for (std::list<std::string>::const_iterator attribute_it = x.attributes.begin(); attribute_it != x.attributes.end(); ++attribute_it)
      {
        arg.SetArgumentAttribute(*attribute_it);
      }
    }
  }

  return true;
}

Generator::Argument& Generator::GetOrAddGlobalArgument(const std::string& name)
{
  std::list<Argument>::iterator it = std::find(globals_.begin(), globals_.end(), name);
  if (it == globals_.end())
  {
    it = globals_.insert(it, Argument(name));
    (*it).pointer = false; // global vars are not pointers by default
  }
  return *it;
}

void Generator::Function::SetArgumentType(const std::string& argument_name, const ast::TypeSpec& type_spec)
{
  std::list<Argument>::iterator argument_it = this->find_argument(argument_name);
  if (argument_it != this->argument_list.end()) (*argument_it).SetArgumentType(type_spec);
  if (boost::iequals(this->return_value.name, argument_name)) this->return_value.SetArgumentType(type_spec);
}
    
void Generator::Function::SetArgumentAttribute(const std::string& argument_name, const std::string& attribute)
{
  std::list<Argument>::iterator argument_it = this->find_argument(argument_name);
  if (argument_it != this->argument_list.end()) (*argument_it).SetArgumentAttribute(attribute);
  if (boost::iequals(this->return_value.name, argument_name)) this->return_value.SetArgumentAttribute(attribute);
}

void Generator::Argument::SetArgumentType(const ast::TypeSpec& type_spec)
{
  this->type = boost::apply_visitor(TypeSpecToCType(), type_spec);
}

void Generator::Argument::SetArgumentAttribute(const std::string& attribute)
{
  if (boost::iequals(attribute, "value")) 
  {
    this->pointer = false;
  }
  if (boost::iequals(attribute, "parameter")) 
  {
    this->parameter = true;
  }
  else if (boost::iequals(attribute, "intentin"))
  {
    this->constant = true;
  }
  else if (boost::iequals(attribute, "intentout"))
  {
    this->constant = false;
  }
  else if (boost::iequals(attribute, "dimension"))
  {
    this->pointer = true;
  }
  else if (boost::algorithm::ifind_first(attribute, "bind").begin() == attribute.begin())
  {
    this->has_c_bind = true;
    this->bind_name = ExtractBindName(attribute);
  }
}

bool Generator::operator()(ast::Program const& x)
{
  BOOST_FOREACH(ast::ProgramBlock const& o, x)
  {
    if (!boost::apply_visitor(*this, o))
    {
      return false;
    }
  }
  return true;
}

std::string Generator::TypeSpecToCType::operator()(ast::TypeSpecIntrinsic const& type_spec) const
{
  std::string result = "unknown_intinsic_type";
  if (boost::iequals(type_spec.keyword, "integer")) result = "int";
  else if (boost::iequals(type_spec.keyword, "real")) result = "double";
  else if (boost::iequals(type_spec.keyword, "character")) result = "char";
  return result;
}

std::string Generator::TypeSpecToCType::operator()(ast::TypeSpecType const& type_spec) const
{
  std::string result = "unknown_derived_type";
  if (boost::iequals(type_spec.type_name, "c_ptr")) result = "void*";
  else result = type_spec.type_name;
  return result;
}

void Generator::DumpHeaderStart(const std::string& define_name, bool omit_comments) const
{
  if (!omit_comments)
  {
    out_ << "// *********************************************************************************************\n";
    out_ << "//                 WARNING: AUTO-GENERATED FILE, DO NOT MODIFY IT.\n";
    out_ << "//\n";
    out_ << "// This file has been generated by fortran_to_c_headers version " << GetVersionString() << "\n";
    out_ << "// For more information visit https://github.com/sharifmarat/fortran_to_c_headers\n";
    out_ << "// *********************************************************************************************\n";
  }

  out_ << "#ifndef " << define_name << "\n";
  out_ << "#define " << define_name << "\n\n";
  out_ << "#ifdef __cplusplus\n";
  out_ << "extern \"C\"\n";
  out_ << "{\n";
  out_ << "#endif\n\n";
}


void Generator::DumpHeaderEnd(const std::string& define_name) const
{
  out_ << "#ifdef __cplusplus\n";
  out_ << "}\n";
  out_ << "#endif\n\n";
  out_ << "#endif //" << define_name << "\n";
}
  
std::string Generator::GetDefineName() const
{
  std::string basename = boost::filesystem::basename(out_file_name_);
  boost::replace_all(basename, " ", "_");
  boost::replace_all(basename, "\"", "_");
  boost::replace_all(basename, "\'", "_");
  boost::to_upper(basename);
  basename = basename + "_H";
  return basename;
}

}
