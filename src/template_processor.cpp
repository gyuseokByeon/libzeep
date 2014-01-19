//            Copyright Maarten L. Hekkelman, 2014.
//   Distributed under the Boost Software License, Version 1.0.
//      (See accompanying file LICENSE_1_0.txt or copy at
//            http://www.boost.org/LICENSE_1_0.txt)
//

#include <zeep/config.hpp>

#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH
#include <boost/algorithm/string.hpp>
#include <boost/date_time/local_time/local_time.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/filesystem/fstream.hpp>

#include <zeep/http/webapp.hpp>
#include <zeep/xml/unicode_support.hpp>
#include <zeep/http/webapp/el.hpp>

using namespace std;
namespace ba = boost::algorithm;
namespace io = boost::iostreams;
namespace fs = boost::filesystem;

namespace zeep { namespace http {

// --------------------------------------------------------------------
//

template_processor::template_processor(const string& ns, const fs::path& docroot)
	: m_ns(ns)
	, m_docroot(docroot)
{
	m_processor_table["include"] =	boost::bind(&template_processor::process_include,	this, _1, _2, _3);
	m_processor_table["if"] =		boost::bind(&template_processor::process_if,		this, _1, _2, _3);
	m_processor_table["iterate"] =	boost::bind(&template_processor::process_iterate,	this, _1, _2, _3);
	m_processor_table["for"] =		boost::bind(&template_processor::process_for,		this, _1, _2, _3);
	m_processor_table["number"] =	boost::bind(&template_processor::process_number,	this, _1, _2, _3);
	m_processor_table["options"] =	boost::bind(&template_processor::process_options,	this, _1, _2, _3);
	m_processor_table["option"] =	boost::bind(&template_processor::process_option,	this, _1, _2, _3);
	m_processor_table["checkbox"] =	boost::bind(&template_processor::process_checkbox,	this, _1, _2, _3);
	m_processor_table["url"] =		boost::bind(&template_processor::process_url, 		this, _1, _2, _3);
	m_processor_table["param"] =	boost::bind(&template_processor::process_param,		this, _1, _2, _3);
	m_processor_table["embed"] =	boost::bind(&template_processor::process_embed,		this, _1, _2, _3);
}

template_processor::~template_processor()
{
}

void template_processor::set_docroot(const fs::path& path)
{
	m_docroot = path;
}

void template_processor::load_template(const string& file, xml::document& doc)
{
	fs::ifstream data(m_docroot / file, ios::binary);
	if (not data.is_open())
	{
		if (not fs::exists(m_docroot))
			throw exception((boost::format("configuration error, docroot not found: '%1%'") % m_docroot).str());
		else
		{
#if defined(_MSC_VER)
			char msg[1024] = "";

		    DWORD dw = ::GetLastError();
			if (dw != NO_ERROR)
			{
			    char* lpMsgBuf;
				int m = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);
			
				if (lpMsgBuf != nullptr)
				{
					// strip off the trailing whitespace characters
					while (m > 0 and isspace(lpMsgBuf[m - 1]))
						--m;
					lpMsgBuf[m] = 0;

					strncpy(msg, lpMsgBuf, sizeof(msg));
		
					::LocalFree(lpMsgBuf);
				}
			}

			throw exception((boost::format("error opening: %1% (%2%)") % (m_docroot / file) % msg).str());
#else
			throw exception((boost::format("error opening: %1% (%2%)") % (m_docroot / file) % strerror(errno)).str());
#endif
		}
	}
	doc.read(data);
}

void template_processor::create_reply_from_template(const string& file, const el::scope& scope, reply& reply)
{
	xml::document doc;
	doc.set_preserve_cdata(true);
	
	load_template(file, doc);
	
	xml::element* root = doc.child();
	process_xml(root, scope, "/");
	reply.set_content(doc);
}

void template_processor::process_xml(xml::node* node, const el::scope& scope, fs::path dir)
{
	xml::text* text = dynamic_cast<xml::text*>(node);
	
	if (text != nullptr)
	{
		string s = text->str();
		if (el::process_el(scope, s))
			text->str(s);
		return;
	}
	
	xml::element* e = dynamic_cast<xml::element*>(node);
	if (e == nullptr)
		return;
	
	// if node is one of our special nodes, we treat it here
	if (e->ns() == m_ns)
	{
		xml::container* parent = e->parent();

		try
		{
			el::scope nested(scope);
			
			processor_map::iterator p = m_processor_table.find(e->name());
			if (p != m_processor_table.end())
				p->second(e, scope, dir);
			else
				throw exception((boost::format("unimplemented <mrs:%1%> tag") % e->name()).str());
		}
		catch (exception& ex)
		{
			xml::node* replacement = new xml::text(
				(boost::format("Error processing directive 'mrs:%1%': %2%") %
					e->name() % ex.what()).str());
			
			parent->insert(e, replacement);
		}

		try
		{
//			assert(parent == e->parent());
//			assert(find(parent->begin(), parent->end(), e) != parent->end());

			parent->remove(e);
			delete e;
		}
		catch (exception& ex)
		{
			cerr << "exception: " << ex.what() << endl;
			cerr << *e << endl;
		}
	}
	else
	{
		foreach (xml::attribute& a, boost::iterator_range<xml::element::attribute_iterator>(e->attr_begin(), e->attr_end()))
		{
			string s = a.value();
			if (process_el(scope, s))
				a.value(s);
		}

		list<xml::node*> nodes;
		copy(e->node_begin(), e->node_end(), back_inserter(nodes));
	
		foreach (xml::node* n, nodes)
		{
			process_xml(n, scope, dir);
		}
	}
}

void template_processor::add_processor(const string& name, processor_type processor)
{
	m_processor_table[name] = processor;
}

void template_processor::process_include(xml::element* node, const el::scope& scope, fs::path dir)
{
	// an include directive, load file and include resulting content
	string file = node->get_attribute("file");
	process_el(scope, file);

	if (file.empty())
		throw exception("missing file attribute");
	
	xml::document doc;
	doc.set_preserve_cdata(true);
	load_template(dir / file, doc);

	xml::element* replacement = doc.child();
	doc.root()->remove(replacement);

	xml::container* parent = node->parent();
	parent->insert(node, replacement);
	
	process_xml(replacement, scope, (dir / file).parent_path());
}

void template_processor::process_if(xml::element* node, const el::scope& scope, fs::path dir)
{
	string test = node->get_attribute("test");
	if (evaluate_el(scope, test))
	{
		foreach (xml::node* c, node->nodes())
		{
			xml::node* clone = c->clone();

			xml::container* parent = node->parent();
			assert(parent);

			parent->insert(node, clone);	// insert before processing, to assign namespaces
			process_xml(clone, scope, dir);
		}
	}
}

void template_processor::process_iterate(xml::element* node, const el::scope& scope, fs::path dir)
{
	el::object collection = scope[node->get_attribute("collection")];
	if (collection.type() != el::object::array_type)
		evaluate_el(scope, node->get_attribute("collection"), collection);
	
	string var = node->get_attribute("var");
	if (var.empty())
		throw exception("missing var attribute in mrs:iterate");

	foreach (el::object& o, collection)
	{
		el::scope s(scope);
		s.put(var, o);
		
		foreach (xml::node* c, node->nodes())
		{
			xml::node* clone = c->clone();

			xml::container* parent = node->parent();
			assert(parent);

			parent->insert(node, clone);	// insert before processing, to assign namespaces
			process_xml(clone, s, dir);
		}
	}
}

void template_processor::process_for(xml::element* node, const el::scope& scope, fs::path dir)
{
	el::object b, e;
	
	evaluate_el(scope, node->get_attribute("begin"), b);
	evaluate_el(scope, node->get_attribute("end"), e);
	
	string var = node->get_attribute("var");
	if (var.empty())
		throw exception("missing var attribute in mrs:iterate");

	for (int32 i = b.as<int32>(); i <= e.as<int32>(); ++i)
	{
		el::scope s(scope);
		s.put(var, el::object(i));
		
		foreach (xml::node* c, node->nodes())
		{
			xml::container* parent = node->parent();
			assert(parent);
			xml::node* clone = c->clone();

			parent->insert(node, clone);	// insert before processing, to assign namespaces
			process_xml(clone, s, dir);
		}
	}
}

class with_thousands : public numpunct<char>
{
  protected:
//	char_type do_thousands_sep() const	{ return tsp; }
	string do_grouping() const			{ return "\03"; }
//	char_type do_decimal_point() const	{ return dsp; }
};

void template_processor::process_number(xml::element* node, const el::scope& scope, fs::path dir)
{
	string number = node->get_attribute("n");
	string format = node->get_attribute("f");
	
	if (format == "#,##0B")	// bytes, convert to a human readable form
	{
		const char kBase[] = { 'B', 'K', 'M', 'G', 'T', 'P', 'E' };		// whatever

		el::object n;
		evaluate_el(scope, number, n);

		uint64 nr = n.as<uint64>();
		int base = 0;

		while (nr > 1024)
		{
			nr /= 1024;
			++base;
		}
		
		locale mylocale(locale(), new with_thousands);
		
		ostringstream s;
		s.imbue(mylocale);
		s.setf(ios::fixed, ios::floatfield);
		s.precision(1);
		s << nr << ' ' << kBase[base];
		number = s.str();
	}
	else if (format.empty() or ba::starts_with(format, "#,##0"))
	{
		el::object n;
		evaluate_el(scope, number, n);
		
		uint64 nr = n.as<uint64>();
		
		locale mylocale(locale(), new with_thousands);
		
		ostringstream s;
		s.imbue(mylocale);
		s << nr;
		number = s.str();
	}

	zeep::xml::node* replacement = new zeep::xml::text(number);
			
	zeep::xml::container* parent = node->parent();
	parent->insert(node, replacement);
}

void template_processor::process_options(xml::element* node, const el::scope& scope, fs::path dir)
{
	el::object collection = scope[node->get_attribute("collection")];
	if (collection.type() != el::object::array_type)
		evaluate_el(scope, node->get_attribute("collection"), collection);
	
	string value = node->get_attribute("value");
	string label = node->get_attribute("label");
	
	string selected = node->get_attribute("selected");
	if (not selected.empty())
	{
		el::object o;
		evaluate_el(scope, selected, o);
		selected = o.as<string>();
	}
	
	foreach (el::object& o, collection)
	{
		zeep::xml::element* option = new zeep::xml::element("option");

		if (not (value.empty() or label.empty()))
		{
			option->set_attribute("value", o[value].as<string>());
			if (selected == o[value].as<string>())
				option->set_attribute("selected", "selected");
			option->add_text(o[label].as<string>());
		}
		else
		{
			option->set_attribute("value", o.as<string>());
			if (selected == o.as<string>())
				option->set_attribute("selected", "selected");
			option->add_text(o.as<string>());
		}

		zeep::xml::container* parent = node->parent();
		assert(parent);
		parent->insert(node, option);
	}
}

void template_processor::process_option(xml::element* node, const el::scope& scope, fs::path dir)
{
	string value = node->get_attribute("value");
	if (not value.empty())
	{
		el::object o;
		evaluate_el(scope, value, o);
		value = o.as<string>();
	}

	string selected = node->get_attribute("selected");
	if (not selected.empty())
	{
		el::object o;
		evaluate_el(scope, selected, o);
		selected = o.as<string>();
	}

	zeep::xml::element* option = new zeep::xml::element("option");

	option->set_attribute("value", value);
	if (selected == value)
		option->set_attribute("selected", "selected");

	zeep::xml::container* parent = node->parent();
	assert(parent);
	parent->insert(node, option);

	foreach (zeep::xml::node* c, node->nodes())
	{
		zeep::xml::node* clone = c->clone();
		option->push_back(clone);
		process_xml(clone, scope, dir);
	}
}

void template_processor::process_checkbox(xml::element* node, const el::scope& scope, fs::path dir)
{
	string name = node->get_attribute("name");
	if (not name.empty())
	{
		el::object o;
		evaluate_el(scope, name, o);
		name = o.as<string>();
	}

	bool checked = false;
	if (not node->get_attribute("checked").empty())
	{
		el::object o;
		evaluate_el(scope, node->get_attribute("checked"), o);
		checked = o.as<bool>();
	}
	
	zeep::xml::element* checkbox = new zeep::xml::element("input");
	checkbox->set_attribute("type", "checkbox");
	checkbox->set_attribute("name", name);
	checkbox->set_attribute("value", "true");
	if (checked)
		checkbox->set_attribute("checked", "true");

	zeep::xml::container* parent = node->parent();
	assert(parent);
	parent->insert(node, checkbox);
	
	foreach (zeep::xml::node* c, node->nodes())
	{
		zeep::xml::node* clone = c->clone();
		checkbox->push_back(clone);
		process_xml(clone, scope, dir);
	}
}

void template_processor::process_url(xml::element* node, const el::scope& scope, fs::path dir)
{
	string var = node->get_attribute("var");
	
	parameter_map parameters;
	get_parameters(scope, parameters);
	
	foreach (zeep::xml::element* e, *node)
	{
		if (e->ns() == m_ns and e->name() == "param")
		{
			string name = e->get_attribute("name");
			string value = e->get_attribute("value");

			process_el(scope, value);
			parameters.replace(name, value);
		}
	}

	string url = scope["baseuri"].as<string>();

	bool first = true;
	foreach (parameter_map::value_type p, parameters)
	{
		if (first)
			url += '?';
		else
			url += '&';
		first = false;

		url += zeep::http::encode_url(p.first) + '=' + zeep::http::encode_url(p.second.as<string>());
	}

	el::scope& s(const_cast<el::scope&>(scope));
	s.put(var, url);
}

void template_processor::process_param(xml::element* node, const el::scope& scope, fs::path dir)
{
	throw exception("Invalid XML, cannot have a stand-alone mrs:param element");
}

void template_processor::process_embed(xml::element* node, const el::scope& scope, fs::path dir)
{
	// an embed directive, load xml from attribute and include parsed content
	string xml = scope[node->get_attribute("var")].as<string>();

	if (xml.empty())
		throw exception("Missing var attribute in embed tag");

	zeep::xml::document doc;
	doc.set_preserve_cdata(true);
	doc.read(xml);

	zeep::xml::element* replacement = doc.child();
	doc.root()->remove(replacement);

	zeep::xml::container* parent = node->parent();
	parent->insert(node, replacement);
	
	process_xml(replacement, scope, dir);
}

void template_processor::init_scope(el::scope& scope)
{
}		

void template_processor::get_parameters(const el::scope& scope, parameter_map& parameters)
{
	const request& req = scope.get_request();
	
	string ps;
	
	if (req.method == "POST")
		ps = req.payload;
	else if (req.method == "GET" or req.method == "PUT")
	{
		string::size_type d = req.uri.find('?');
		if (d != string::npos)
			ps = req.uri.substr(d + 1);
	}

	while (not ps.empty())
	{
		string::size_type e = ps.find_first_of("&;");
		string param;
		
		if (e != string::npos)
		{
			param = ps.substr(0, e);
			ps.erase(0, e + 1);
		}
		else
			swap(param, ps);
		
		if (not param.empty())
			parameters.add(param);
	}
}

// --------------------------------------------------------------------
//

// add a name/value pair as a string formatted as 'name=value'
void parameter_map::add(
	const string&		param)
{
	string name, value;
	
	string::size_type d = param.find('=');
	if (d != string::npos)
	{
		name = param.substr(0, d);
		value = param.substr(d + 1);
	}
	
	add(name, value);
}

void parameter_map::add(
	string		name,
	string		value)
{
	name = decode_url(name);
	if (not value.empty())
		value = decode_url(value);
	
	insert(make_pair(name, parameter_value(value, false)));
}

void parameter_map::replace(
	string			name,
	string			value)
{
	if (count(name))
		erase(lower_bound(name), upper_bound(name));
	add(name, value);
}

}
}

