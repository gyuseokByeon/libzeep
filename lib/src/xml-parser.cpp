// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
//  Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <sstream>
#include <map>
#include <cassert>
#include <memory>
#include <algorithm>

#include <boost/algorithm/string.hpp>

#include <zeep/exception.hpp>
#include <zeep/xml/parser.hpp>
#include <zeep/xml/doctype.hpp>
#include <zeep/xml/unicode_support.hpp>

#include "parser-internals.hpp"

namespace ba = boost::algorithm;

namespace zeep
{
namespace xml
{

using namespace ::zeep::detail;

// --------------------------------------------------------------------

class entity_data_source : public string_data_source
{
public:
	entity_data_source(const std::string& entity_name, const std::string& entity_path,
					   const std::string& text, data_source *next = nullptr)
		: string_data_source(text, next), m_entity_name(entity_name)
	{
		base(entity_path);
	}

	virtual bool is_entity_on_stack(const std::string& name)
	{
		bool result = m_entity_name == name;
		if (result == false and m_next != nullptr)
			result = m_next->is_entity_on_stack(name);
		return result;
	}

protected:
	std::string m_entity_name;
};

// --------------------------------------------------------------------

class parameter_entity_data_source : public string_data_source
{
public:
	parameter_entity_data_source(const std::string& data, const std::string& base_dir, data_source *next = nullptr)
		: string_data_source(std::string(" ") + data + " ", next)
	{
		base(base_dir);
	}

	virtual bool auto_discard() const { return m_next != nullptr; }
};

// --------------------------------------------------------------------

class valid_nesting_validator
{
public:
	valid_nesting_validator(data_source *source)
		: m_id(source->id()) {}

	void check(data_source *source)
	{
		if (source->id() != m_id)
			throw invalid_exception("proper nesting validation error");
	}

private:
	int m_id;
};

// --------------------------------------------------------------------

struct parser_imp
{
	parser_imp(std::istream& data, parser& parser);

	~parser_imp();

	// Here comes the parser part
	void parse(bool validate);

	// the productions. Some are inlined below for obvious reasons.
	// names of the productions try to follow those in the TR http://www.w3.org/TR/xml
	void prolog();
	void xml_decl();
	void text_decl();

	void s(bool at_least_one = false);
	void eq();
	void misc();
	void element(doctype::validator& valid);
	void content(doctype::validator& valid, bool check_for_whitespace);

	void comment();
	void pi();

	void pereference();

	void doctypedecl();
	data_source *external_id();
	std::tuple<std::string, std::string>
	read_external_id();
	void intsubset();
	void extsubset();
	void declsep();
	void conditionalsect();
	void ignoresectcontents();
	void markup_decl();
	void element_decl();
	void contentspec(doctype::element& element);
	doctype::allowed_ptr
	cp();
	void attlist_decl();
	void notation_decl();
	void entity_decl();
	void parameter_entity_decl();
	void general_entity_decl();
	void entity_value();

	// at several locations we need to parse out entity references from strings:
	void parse_parameter_entity_declaration(std::string& s);
	void parse_general_entity_declaration(std::string& s);

	// same goes for attribute values
	std::string normalize_attribute_value(const std::string& s)
	{
		string_data_source data(s);
		return normalize_attribute_value(&data);
	}

	std::string normalize_attribute_value(data_source *data);

	// The scanner is next. We recognize the following tokens:
	enum XMLToken
	{
		xml_Undef,
		xml_Eof = 256,

		// these are tokens for the markup

		xml_XMLDecl,  // <?xml
		xml_Space,	// Really needed
		xml_Comment,  // <!--
		xml_Name,	 // name-start-char (name-char)*
		xml_NMToken,  // (name-char)+
		xml_String,   // (\"[^"]*\") | (\'[^\']*\')		// single or double quoted std::string
		xml_PI,		  // <?
		xml_STag,	 // <
		xml_ETag,	 // </
		xml_DocType,  // <!DOCTYPE
		xml_Element,  // <!ELEMENT
		xml_AttList,  // <!ATTLIST
		xml_Entity,   // <!ENTITY
		xml_Notation, // <!NOTATION

		xml_IncludeIgnore, // <![

		xml_PEReference, // %name;

		// next are tokens for the content part

		xml_Reference, // &name;
		xml_CDSect,	// CData section <![CDATA[ ... ]]>
		xml_Content,   // anything else up to the next element start
	};

	// for debugging and error reporting we have the following describing routine
	std::string describe_token(int token);

	unicode get_next_char();

	// Recognizing tokens differs if we are expecting markup or content in elements:
	int get_next_token();
	int get_next_content();

	// retract is used when we've read a character too much from the input stream
	void retract();

	// match, check if the look-a-head token is really what we expect here.
	// throws if it isn't. Depending on the content flag we call either get_next_token or get_next_content
	// to find the next look-a-head token.
	void match(int token);

	// utility routine
	float parse_version();

	// error handling routines
	void not_well_formed(const std::string& msg) const;
	// void			not_well_formed(const boost::format& msg) const			{ not_well_formed(msg.str()); }
	void not_valid(const std::string& msg) const;
	// void			not_valid(const boost::format& msg) const				{ not_valid(msg.str()); }

	// doctype support
	const doctype::entity& get_general_entity(const std::string& name) const;
	const doctype::entity& get_parameter_entity(const std::string& name) const;
	const doctype::element *get_element(const std::string& name) const;

	// Sometimes we need to reuse our parser/scanner to parse an external entity e.g.
	// We use stack based state objects to store the current state.
	struct parser_state
	{
		parser_state(parser_imp *imp, data_source *source)
			: m_impl(imp), m_lookahead(0), m_data_source(source), m_version(1.0f), m_encoding(encoding_type::enc_UTF8), m_external_subset(true), m_external_dtd(false)
		{
			swap_state();
		}

		~parser_state()
		{
			swap_state();

			if (m_data_source != nullptr and m_data_source->auto_discard())
				delete m_data_source;
		}

		void swap_state()
		{
			std::swap(m_impl->m_lookahead, m_lookahead);
			std::swap(m_impl->m_token, m_token);
			std::swap(m_impl->m_data_source, m_data_source);
			std::swap(m_impl->m_buffer, m_buffer);
			std::swap(m_impl->m_version, m_version);
			std::swap(m_impl->m_encoding, m_encoding);
			std::swap(m_impl->m_external_subset, m_external_subset);
			std::swap(m_impl->m_in_external_dtd, m_external_dtd);
		}

		parser_imp *m_impl;
		int m_lookahead;
		data_source *m_data_source;
		mini_stack m_buffer;
		std::string m_token;
		float m_version;
		encoding_type m_encoding;
		bool m_external_subset;
		bool m_external_dtd;
	};

	// And during parsing we keep track of the namespaces we encounter.
	struct ns_state
	{
		ns_state(parser_imp *imp)
			: m_parser_imp(imp), m_next(imp->m_ns)
		{
			m_parser_imp->m_ns = this;
		}

		~ns_state()
		{
			m_parser_imp->m_ns = m_next;
		}

		parser_imp *m_parser_imp;
		std::string m_default_ns;
		ns_state *m_next;

		std::map<std::string, std::string>
			m_known;

		std::string default_ns()
		{
			std::string result = m_default_ns;
			if (result.empty() and m_next != nullptr)
				result = m_next->default_ns();
			return result;
		}

		std::string ns_for_prefix(const std::string& prefix)
		{
			std::string result;

			if (m_known.find(prefix) != m_known.end())
				result = m_known[prefix];
			else if (m_next != nullptr)
				result = m_next->ns_for_prefix(prefix);

			return result;
		}
	};

	bool m_validating;
	bool m_has_dtd;
	int m_lookahead;
	data_source *m_data_source;
	mini_stack m_buffer;
	std::string m_token;
	float m_version;
	encoding_type m_encoding;
	bool m_standalone;
	parser& m_parser;
	ns_state *m_ns;
	bool m_in_doctype; // used to keep track where we are (parameter entities are only recognized inside a doctype section)
	bool m_external_subset;
	bool m_in_element;
	bool m_in_content;
	bool m_in_external_dtd;
	bool m_allow_parameter_entity_references;

	std::string m_root_element;
	doctype::entity_list m_parameter_entities;
	doctype::entity_list m_general_entities;
	doctype::element_list m_doctype;

	std::set<std::string> m_notations;
	std::set<std::string> m_ids;			// attributes of type ID should be unique
	std::set<std::string> m_unresolved_ids; // keep track of IDREFS that were not found yet
};

// --------------------------------------------------------------------
// some inlines

inline void parser_imp::s(bool at_least_one)
{
	if (at_least_one)
		match(xml_Space);

	while (m_lookahead == xml_Space)
		match(xml_Space);
}

inline void parser_imp::eq()
{
	s();
	match('=');
	s();
}

// --------------------------------------------------------------------

parser_imp::parser_imp(
	std::istream& data,
	parser& parser)
	: m_validating(true), m_has_dtd(false), m_lookahead(xml_Eof), m_data_source(new istream_data_source(data, nullptr)), m_version(1.0f), m_standalone(false), m_parser(parser), m_ns(nullptr), m_in_doctype(false), m_external_subset(false), m_in_element(false), m_in_content(false), m_in_external_dtd(false), m_allow_parameter_entity_references(false)
{
	m_token.reserve(10000);

	m_encoding = m_data_source->encoding();

	// these entities are always recognized:
	m_general_entities.push_back(new doctype::general_entity("lt", "&#60;"));
	m_general_entities.push_back(new doctype::general_entity("gt", "&#62;"));
	m_general_entities.push_back(new doctype::general_entity("amp", "&#38;"));
	m_general_entities.push_back(new doctype::general_entity("apos", "&#39;"));
	m_general_entities.push_back(new doctype::general_entity("quot", "&#34;"));
}

parser_imp::~parser_imp()
{
	// there may be parameter_entity_data_source's left in the stack
	// as a result of a validation error/exception
	while (m_data_source->auto_discard())
	{
		data_source *next = m_data_source->next_data_source();
		delete m_data_source;
		m_data_source = next;
	}

	for (doctype::entity *e : m_parameter_entities)
		delete e;

	for (doctype::entity *e : m_general_entities)
		delete e;

	for (doctype::element *e : m_doctype)
		delete e;

	delete m_data_source;
}

const doctype::entity& parser_imp::get_general_entity(const std::string& name) const
{
	auto e = std::find_if(m_general_entities.begin(), m_general_entities.end(),
						  [name](auto e) { return e->name() == name; });

	if (e == m_general_entities.end())
		not_well_formed("undefined entity reference '" + name + "'");

	return **e;
}

const doctype::entity& parser_imp::get_parameter_entity(const std::string& name) const
{
	auto e = find_if(m_parameter_entities.begin(), m_parameter_entities.end(),
					 [name](auto e) { return e->name() == name; });

	if (e == m_parameter_entities.end())
	{
		std::string msg = "Undefined parameter entity '" + m_token + '\'';

		if (m_standalone)
			not_well_formed(msg);
		else
			not_valid(msg);

		// should not happen...
		throw zeep::exception(msg);
	}

	return **e;
}

const doctype::element *parser_imp::get_element(const std::string& name) const
{
	const doctype::element *result = nullptr;

	auto e = find_if(m_doctype.begin(), m_doctype.end(),
					 [name](auto e) { return e->name() == name; });

	if (e != m_doctype.end())
		result = *e;

	return result;
}

unicode parser_imp::get_next_char()
{
	unicode result = 0;

	if (not m_buffer.empty()) // if buffer is not empty we already did all the validity checks
	{
		result = m_buffer.top();
		m_buffer.pop();
	}
	else
	{
		for (;;)
		{
			try
			{
				result = m_data_source->get_next_char();
			}
			catch (source_exception& e)
			{
				not_well_formed(e.m_wmsg);
			}

			if (result == 0 and m_data_source->auto_discard())
			{
				data_source *next = m_data_source->next_data_source();
				delete m_data_source;
				m_data_source = next;

				if (m_data_source != nullptr)
					continue;
			}

			break;
		}

		if (result >= 0x080)
		{
			if (result == 0x0ffff or result == 0x0fffe)
				not_well_formed("character " + to_hex(result) + " is not allowed");

			// surrogate support
			else if (result >= 0x0D800 and result <= 0x0DBFF)
			{
				unicode uc2 = get_next_char();
				if (uc2 >= 0x0DC00 and uc2 <= 0x0DFFF)
					result = (result - 0x0D800) * 0x400 + (uc2 - 0x0DC00) + 0x010000;
				else
					not_well_formed("leading surrogate character without trailing surrogate character");
			}
			else if (result >= 0x0DC00 and result <= 0x0DFFF)
				not_well_formed("trailing surrogate character without a leading surrogate");
		}
	}

	//	append(m_token, result);
	// somehow, append refuses to inline, so we have to do it ourselves
	if (result < 0x080)
		m_token += (static_cast<char>(result));
	else if (result < 0x0800)
	{
		char ch[2] = {
			static_cast<char>(0x0c0 | (result >> 6)),
			static_cast<char>(0x080 | (result & 0x3f))};
		m_token.append(ch, 2);
	}
	else if (result < 0x00010000)
	{
		char ch[3] = {
			static_cast<char>(0x0e0 | (result >> 12)),
			static_cast<char>(0x080 | ((result >> 6) & 0x3f)),
			static_cast<char>(0x080 | (result & 0x3f))};
		m_token.append(ch, 3);
	}
	else
	{
		char ch[4] = {
			static_cast<char>(0x0f0 | (result >> 18)),
			static_cast<char>(0x080 | ((result >> 12) & 0x3f)),
			static_cast<char>(0x080 | ((result >> 6) & 0x3f)),
			static_cast<char>(0x080 | (result & 0x3f))};
		m_token.append(ch, 4);
	}

	return result;
}

void parser_imp::retract()
{
	assert(not m_token.empty());
	m_buffer.push(pop_last_char(m_token));
}

void parser_imp::match(int token)
{
	if (m_lookahead != token)
	{
		std::string expected = describe_token(token);
		std::string found = describe_token(m_lookahead);

		not_well_formed(
			"Error parsing XML, expected '" + expected + "' but found '" + found + "' ('" + m_token + "')");
	}

	if (m_in_content)
		m_lookahead = get_next_content();
	else
	{
		m_lookahead = get_next_token();

		if (m_lookahead == xml_PEReference and m_allow_parameter_entity_references)
			pereference();
	}
}

void parser_imp::not_well_formed(const std::string& msg) const
{
	std::stringstream s;
	s << "Document (line: " << m_data_source->get_line_nr() << ") not well-formed: " << msg;
	throw not_wf_exception(s.str());
}

void parser_imp::not_valid(const std::string& msg) const
{
	if (m_validating)
	{
		std::stringstream s;
		s << "Document (line: " << m_data_source->get_line_nr() << ") invalid: " << msg;
		throw invalid_exception(s.str());
	}
	else
		m_parser.report_invalidation(msg);
}

/*
	get_next_token is a hand optimised scanner for tokens in the input stream.
*/

int parser_imp::get_next_token()
{
	enum State
	{
		state_Start = 0,
		state_WhiteSpace = 10,
		state_Tag = 20,
		state_String = 30,
		state_PERef = 40,
		state_Name = 50,
		state_CommentOrDoctype = 60,
		state_Comment = 70,
		state_DocTypeDecl = 80,
		state_PI = 90,
	};

	int token = xml_Undef;
	unicode quote_char = 0;
	int state = state_Start;
	bool might_be_name = false;

	m_token.clear();

	while (token == xml_Undef)
	{
		unicode uc = get_next_char();

		switch (state)
		{
		// start scanning.
		case state_Start:
			if (uc == 0)
				token = xml_Eof;
			else if (uc == ' ' or uc == '\t' or uc == '\n')
				state = state_WhiteSpace;
			else if (uc == '<')
				state = state_Tag;
			else if (uc == '\'' or uc == '\"')
			{
				state = state_String;
				quote_char = uc;
			}
			else if (uc == '%')
				state = state_PERef;
			else if (is_name_start_char(uc))
			{
				might_be_name = true;
				state = state_Name;
			}
			else if (is_name_char(uc))
				state = state_Name;
			else
				token = uc;
			break;

		// collect all whitespace
		case state_WhiteSpace:
			if (uc != ' ' and uc != '\t' and uc != '\n')
			{
				retract();
				token = xml_Space;
			}
			break;

		// We scanned a < character, decide what to do next.
		case state_Tag:
			if (uc == '!') // comment or doctype thing
				state = state_CommentOrDoctype;
			else if (uc == '/') // end tag
				token = xml_ETag;
			else if (uc == '?') // processing instruction
				state = state_PI;
			else // anything else
			{
				retract();
				token = xml_STag;
			}
			break;

		// So we had <! which can only be followed validly by '-', '[' or a character at the current location
		case state_CommentOrDoctype:
			if (uc == '-')
				state = state_Comment;
			else if (uc == '[' and m_external_subset)
				token = xml_IncludeIgnore;
			else if (is_name_start_char(uc))
				state = state_DocTypeDecl;
			else
				not_well_formed("Unexpected character");
			break;

		// Comment, strictly check for <!-- -->
		case state_Comment:
			if (uc == '-')
				token = xml_Comment;
			else
				not_well_formed("Invalid formatted comment");
			break;

		// scan for processing instructions
		case state_PI:
			if (not is_name_char(uc))
			{
				retract();

				// we treat the xml processing instruction separately.
				if (m_token.substr(2) == "xml")
					token = xml_XMLDecl;
				else if (iequals(m_token.substr(2), "xml"))
					not_well_formed("<?XML is neither an XML declaration nor a legal processing instruction target");
				else
					token = xml_PI;
			}
			break;

		// One of the DOCTYPE tags. We scanned <!(char), continue until non-char
		case state_DocTypeDecl:
			if (not is_name_char(uc))
			{
				retract();

				if (m_token == "<!DOCTYPE")
					token = xml_DocType;
				else if (m_token == "<!ELEMENT")
					token = xml_Element;
				else if (m_token == "<!ATTLIST")
					token = xml_AttList;
				else if (m_token == "<!ENTITY")
					token = xml_Entity;
				else if (m_token == "<!NOTATION")
					token = xml_Notation;
				else
					not_well_formed("invalid doctype declaration '" + m_token + "'");
			}
			break;

		// strings
		case state_String:
			if (uc == quote_char)
			{
				token = xml_String;
				m_token = m_token.substr(1, m_token.length() - 2);
			}
			else if (uc == 0)
				not_well_formed("unexpected end of file, runaway std::string");
			break;

		// Names
		case state_Name:
			if (not is_name_char(uc))
			{
				retract();

				if (might_be_name)
					token = xml_Name;
				else
					token = xml_NMToken;
			}
			break;

		// parameter entity references
		case state_PERef:
			if (is_name_start_char(uc))
				state += 1;
			else
			{
				retract();
				token = '%';
			}
			break;

		case state_PERef + 1:
			if (uc == ';')
			{
				m_token = m_token.substr(1, m_token.length() - 2);
				token = xml_PEReference;
			}
			else if (not is_name_char(uc))
				not_well_formed("invalid parameter entity reference");
			break;

		default:
			assert(false);
			not_well_formed("state should never be reached");
		}
	}

	//#if DEBUG
	//	if (VERBOSE)
	//		cout << "token: " << describe_token(token) << " (" << m_token << ')' << endl;
	//#endif

	return token;
}

int parser_imp::get_next_content()
{
	enum State
	{
		state_Start = 10,
		state_Tag = 20,
		state_Reference = 30,
		state_WhiteSpace = 40,
		state_Content = 50,
		state_PI = 60,
		state_CommentOrCDATA = 70,
		state_Comment = 80,
		state_CDATA = 90,
		state_Illegal = 100
	};

	int token = xml_Undef;
	int state = state_Start;
	unicode charref = 0;

	m_token.clear();

	while (token == xml_Undef)
	{
		unicode uc = get_next_char();

		if (uc != 0 and not is_char(uc))
			not_well_formed("illegal character in content: '" + to_hex(uc) + "'");

		switch (state)
		{
		case state_Start:
			if (uc == 0)
				token = xml_Eof; // end of file reached
			else if (uc == '<')
				state = state_Tag; // beginning of a tag
			else if (uc == '&')
				state = state_Reference; // a& reference;
			else if (uc == ']')
				state = state_Illegal; // avoid ]]> in text
			else if (is_char(uc))
				state = state_Content; // anything else
			break;

		// content. Only stop collecting character when uc is special
		case state_Content:
			if (uc == ']')
				state = state_Illegal;
			else if (uc == 0 or uc == '<' or uc == '&')
			{
				retract();
				token = xml_Content;
			}
			else if (not is_char(uc))
				not_well_formed("Illegal character in content text");
			break;

		// beginning of a tag?
		case state_Tag:
			if (uc == '/')
				token = xml_ETag;
			else if (uc == '?') // processing instruction
				state = state_PI;
			else if (uc == '!') // comment or CDATA
				state = state_CommentOrCDATA;
			else
			{
				retract();
				token = xml_STag;
			}
			break;

		// processing instructions
		case state_PI:
			if (not is_name_char(uc))
			{
				retract();
				token = xml_PI;
			}
			break;

		// comment or CDATA
		case state_CommentOrCDATA:
			if (uc == '-') // comment
				state = state_Comment;
			else if (uc == '[')
				state = state_CDATA; // CDATA
			else
				not_well_formed("invalid content");
			break;

		case state_Comment:
			if (uc == '-')
				token = xml_Comment;
			else
				not_well_formed("invalid content");
			break;

		// CDATA (we parsed <![ up to this location
		case state_CDATA:
			if (is_name_start_char(uc))
				state += 1;
			else
				not_well_formed("invalid content");
			break;

		case state_CDATA + 1:
			if (uc == '[' and m_token == "<![CDATA[")
				state += 1;
			else if (not is_name_char(uc))
				not_well_formed("invalid content");
			break;

		case state_CDATA + 2:
			if (uc == ']')
				state += 1;
			else if (uc == 0)
				not_well_formed("runaway cdata section");
			break;

		case state_CDATA + 3:
			if (uc == ']')
				state += 1;
			else if (uc == 0)
				not_well_formed("runaway cdata section");
			else if (uc != ']')
				state = state_CDATA + 2;
			break;

		case state_CDATA + 4:
			if (uc == '>')
			{
				token = xml_CDSect;
				m_token = m_token.substr(9, m_token.length() - 12);
			}
			else if (uc == 0)
				not_well_formed("runaway cdata section");
			else if (uc != ']')
				state = state_CDATA + 2;
			break;

		// reference, either a character reference or a general entity reference
		case state_Reference:
			if (uc == '#')
				state = state_Reference + 2;
			else if (is_name_start_char(uc))
				state = state_Reference + 1;
			else
				not_well_formed("stray ampersand found in content");
			break;

		case state_Reference + 1:
			if (not is_name_char(uc))
			{
				if (uc != ';')
					not_well_formed("invalid entity found in content, missing semicolon?");
				token = xml_Reference;
				m_token = m_token.substr(1, m_token.length() - 2);
			}
			break;

		case state_Reference + 2:
			if (uc == 'x')
				state = state_Reference + 4;
			else if (uc >= '0' and uc <= '9')
			{
				charref = uc - '0';
				state += 1;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_Reference + 3:
			if (uc >= '0' and uc <= '9')
				charref = charref * 10 + (uc - '0');
			else if (uc == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character in content text");
				m_token.clear();
				append(m_token, charref);
				token = xml_Content;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_Reference + 4:
			if (uc >= 'a' and uc <= 'f')
			{
				charref = uc - 'a' + 10;
				state += 1;
			}
			else if (uc >= 'A' and uc <= 'F')
			{
				charref = uc - 'A' + 10;
				state += 1;
			}
			else if (uc >= '0' and uc <= '9')
			{
				charref = uc - '0';
				state += 1;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_Reference + 5:
			if (uc >= 'a' and uc <= 'f')
				charref = (charref << 4) + (uc - 'a' + 10);
			else if (uc >= 'A' and uc <= 'F')
				charref = (charref << 4) + (uc - 'A' + 10);
			else if (uc >= '0' and uc <= '9')
				charref = (charref << 4) + (uc - '0');
			else if (uc == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character in content text");
				m_token.clear();
				append(m_token, charref);
				token = xml_Content;
			}
			else
				not_well_formed("invalid character reference");
			break;

		// ]]> is illegal
		case state_Illegal:
			if (uc == ']')
				state += 1;
			else
			{
				retract();
				state = state_Content;
			}
			break;

		case state_Illegal + 1:
			if (uc == '>')
				not_well_formed("the sequence ']]>' is illegal in content text");
			else if (uc != ']')
			{
				retract();
				retract();
				state = state_Content;
			}
			break;

		default:
			assert(false);
			not_well_formed("state reached that should not be reachable");
		}
	}

	//#if DEBUG
	//	if (VERBOSE)
	//		cout << "content: " << describe_token(token) << " (" << m_token << ')' << endl;
	//#endif

	return token;
}

std::string parser_imp::describe_token(int token)
{
	std::string result;

	if (token > xml_Undef and token < xml_Eof)
	{
		std::stringstream s;

		if (isprint(token))
			s << char(token);
		else
			s << "&#x" << std::hex << token << ';';

		result = s.str();
	}
	else
	{
		switch (XMLToken(token))
		{
		case xml_Undef:
			result = "undefined";
			break;
		case xml_Eof:
			result = "end of file";
			break;
		case xml_XMLDecl:
			result = "'<?xml'";
			break;
		case xml_Space:
			result = "space character";
			break;
		case xml_Comment:
			result = "comment";
			break;
		case xml_Name:
			result = "identifier or name";
			break;
		case xml_NMToken:
			result = "nmtoken";
			break;
		case xml_String:
			result = "quoted std::string";
			break;
		case xml_PI:
			result = "processing instruction";
			break;
		case xml_STag:
			result = "tag";
			break;
		case xml_ETag:
			result = "end tag";
			break;
		case xml_DocType:
			result = "<!DOCTYPE";
			break;
		case xml_Element:
			result = "<!ELEMENT";
			break;
		case xml_AttList:
			result = "<!ATTLIST";
			break;
		case xml_Entity:
			result = "<!ENTITY";
			break;
		case xml_Notation:
			result = "<!NOTATION";
			break;
		case xml_PEReference:
			result = "parameter entity reference";
			break;
		case xml_Reference:
			result = "entity reference";
			break;
		case xml_CDSect:
			result = "CDATA section";
			break;
		case xml_Content:
			result = "content";
			break;

		case xml_IncludeIgnore:
			result = "<![ (as in <![INCLUDE[ )";
			break;
		}
	}

	return result;
}

float parser_imp::parse_version()
{
	float result = -1;

	if (m_token.length() >= 3)
	{
		std::string::const_iterator i = m_token.begin();
		if (*i == '1' and *(i + 1) == '.')
		{
			result = 1.0f;
			float dec = 10;

			for (i += 2; i != m_token.end(); ++i)
			{
				if (*i < '0' or *i > '9')
				{
					result = -1;
					break;
				}

				result += (*i - '0') / dec;
				dec += 10;
			}
		}
	}

	if (result < 1.0 or result >= 2.0)
		not_well_formed("Invalid version specified: '" + m_token + "'");

	return result;
}

void parser_imp::parse(bool validate)
{
	m_validating = validate;

	m_lookahead = get_next_token();

	prolog();

	doctype::validator valid;

	const doctype::element *e = get_element(m_root_element);

	if (m_has_dtd and e == nullptr and m_validating)
		not_valid("Element '" + m_root_element + "' is not defined in DTD");

	std::unique_ptr<doctype::allowed_element> allowed(new doctype::allowed_element(m_root_element));

	if (e != nullptr)
		valid = doctype::validator(allowed.get());

	element(valid);
	misc();

	if (m_lookahead != xml_Eof)
		not_well_formed("garbage at end of file");

	if (not m_unresolved_ids.empty())
	{
		not_valid("document contains references to the following undefined ID's: '" + ba::join(m_unresolved_ids, ", ") + "'");
	}
}

void parser_imp::prolog()
{
	xml_decl();

	misc();

	if (m_lookahead == xml_DocType)
	{
		doctypedecl();
		misc();
	}
	else if (m_validating)
		not_valid("document type declaration is missing");
}

void parser_imp::xml_decl()
{
	if (m_lookahead == xml_XMLDecl)
	{
		match(xml_XMLDecl);

		s(true);
		if (m_token != "version")
			not_well_formed("expected a version attribute in XML declaration");
		match(xml_Name);
		eq();
		m_version = parse_version();
		if (m_version >= 2.0 or m_version < 1.0)
			not_well_formed("This library only supports XML version 1.x");
		match(xml_String);

		if (m_lookahead == xml_Space)
		{
			s(true);

			if (m_token == "encoding")
			{
				match(xml_Name);
				eq();
				ba::to_upper(m_token);
				if (m_token == "UTF-8" or m_token == "US-ASCII") // ascii is a subset of utf-8
					m_encoding = encoding_type::enc_UTF8;
				else if (m_token == "UTF-16")
				{
					if (m_encoding != encoding_type::enc_UTF16LE and m_encoding != encoding_type::enc_UTF16BE)
						not_well_formed("Inconsistent encoding attribute in XML declaration");
					//						cerr << "Inconsistent encoding attribute in XML declaration" << endl;
					m_encoding = encoding_type::enc_UTF16BE;
				}
				else if (m_token == "ISO-8859-1")
					m_encoding = encoding_type::enc_ISO88591;
				else
					not_well_formed("Unsupported encoding value '" + m_token + "'");
				match(xml_String);

				s();
			}

			if (m_token == "standalone")
			{
				match(xml_Name);
				eq();
				if (m_token != "yes" and m_token != "no")
					not_well_formed("Invalid XML declaration, standalone value should be either yes or no");
				m_standalone = (m_token == "yes");
				match(xml_String);
				s();
			}
		}

		match('?');
		match('>');
	}
}

void parser_imp::text_decl()
{
	if (m_lookahead == xml_XMLDecl)
	{
		match(xml_XMLDecl);

		s(true);

		if (m_token == "version")
		{
			match(xml_Name);
			eq();
			m_version = parse_version();
			if (m_version >= 2.0 or m_version < 1.0)
				throw exception("This library only supports XML version 1.x");
			match(xml_String);
			s(true);
		}

		if (m_token != "encoding")
			not_well_formed("encoding attribute is mandatory in text declaration");
		match(xml_Name);
		eq();
		match(xml_String);
		s();

		match('?');
		match('>');
	}
}

void parser_imp::misc()
{
	for (;;)
	{
		switch (m_lookahead)
		{
		case xml_Space:
			s();
			continue;

		case xml_Comment:
			comment();
			continue;

		case xml_PI:
			pi();
			continue;
		}

		break;
	}
}

void parser_imp::doctypedecl()
{
	value_saver<bool> in_doctype(m_in_doctype, true);
	value_saver<bool> allow_parameter_entity_references(m_allow_parameter_entity_references, false);

	match(xml_DocType);

	m_has_dtd = true;

	s(true);

	std::string name = m_token;
	match(xml_Name);

	m_root_element = name;

	std::unique_ptr<data_source> dtd;

	if (m_lookahead == xml_Space)
	{
		s(true);

		if (m_lookahead == xml_Name)
		{
			dtd.reset(external_id());
			match(xml_String);
		}

		s();
	}

	if (m_lookahead == '[')
	{
		match('[');
		intsubset();
		match(']');

		s();
	}

	// internal subset takes precedence over external subset, so
	// if the external subset is defined, include it here.
	if (dtd.get() != nullptr)
	{
		// save the parser state
		parser_state save(this, dtd.get());

		m_data_source = dtd.get();
		m_lookahead = get_next_token();
		m_in_external_dtd = true;

		text_decl();

		extsubset();

		if (m_lookahead != xml_Eof)
			not_well_formed("Error parsing external dtd");

		m_in_external_dtd = false;
	}

	match('>');

	// test if all ndata references can be resolved

	for (const doctype::entity *e : m_general_entities)
	{
		if (e->parsed() == false and m_notations.count(e->ndata()) == 0)
			not_valid("Undefined NOTATION '" + e->ndata() + "'");
	}

	// and the notations in the doctype attlists
	for (const doctype::element *element : m_doctype)
	{
		for (const doctype::attribute *attr : element->attributes())
		{
			if (attr->get_type() != doctype::attTypeNotation)
				continue;

			for (const std::string& n : attr->get_enums())
			{
				if (m_notations.count(n) == 0)
					not_valid("Undefined NOTATION '" + n + "'");
			}
		}
	}
}

void parser_imp::pereference()
{
	const doctype::entity& e = get_parameter_entity(m_token);

	m_data_source = new parameter_entity_data_source(e.replacement(), e.path(), m_data_source);

	match(xml_PEReference);
}

void parser_imp::intsubset()
{
	value_saver<bool> allow_parameter_entity_references(m_allow_parameter_entity_references, false);

	for (;;)
	{
		switch (m_lookahead)
		{
		case xml_Element:
		case xml_AttList:
		case xml_Entity:
		case xml_Notation:
			markup_decl();
			continue;

		case xml_PI:
			pi();
			continue;

		case xml_Comment:
			comment();
			continue;

		case xml_Space:
		case xml_PEReference:
			declsep();
			continue;
		}

		break;
	}
}

void parser_imp::declsep()
{
	switch (m_lookahead)
	{
	case xml_PEReference:
	{
		const doctype::entity& e = get_parameter_entity(m_token);

		{
			parameter_entity_data_source source(e.replacement(), e.path());
			parser_state state(this, &source);

			m_lookahead = get_next_token();
			extsubset();
			if (m_lookahead != xml_Eof)
				not_well_formed("parameter entity replacement should match external subset production");
		}

		match(xml_PEReference);
		break;
	}

	case xml_Space:
		s();
		break;
	}
}

void parser_imp::extsubset()
{
	value_saver<bool> save(m_external_subset, true);
	value_saver<bool> allow_parameter_entity_references(m_allow_parameter_entity_references, false);

	for (;;)
	{
		switch (m_lookahead)
		{
		case xml_Element:
		case xml_AttList:
		case xml_Entity:
		case xml_Notation:
			markup_decl();
			continue;

		case xml_IncludeIgnore:
			conditionalsect();
			continue;

		case xml_PI:
			pi();
			continue;

		case xml_Comment:
			comment();
			continue;

		case xml_Space:
		case xml_PEReference:
			declsep();
			continue;
		}

		break;
	}
}

void parser_imp::conditionalsect()
{
	valid_nesting_validator check(m_data_source);
	match(xml_IncludeIgnore);

	s();

	bool include = false;

	if (m_lookahead == xml_PEReference)
	{
		pereference();
		s();
	}

	if (m_token == "INCLUDE")
		include = true;
	else if (m_token == "IGNORE")
		include = false;
	else if (m_lookahead == xml_Name)
		not_well_formed("Unexpected literal '" + m_token + "'");

	match(xml_Name);

	check.check(m_data_source);

	s();

	if (include)
	{
		match('[');
		extsubset();
		match(']');
		match(']');
		check.check(m_data_source);
		match('>');
	}
	else
	{
		ignoresectcontents();
		check.check(m_data_source);
		m_lookahead = get_next_token();
	}
}

void parser_imp::ignoresectcontents()
{
	// yet another tricky routine, skip

	int state = 0;
	bool done = false;

	while (not done)
	{
		unicode ch = get_next_char();
		if (ch == 0)
			not_well_formed("runaway IGNORE section");

		switch (state)
		{
		case 0:
			if (ch == ']')
				state = 1;
			else if (ch == '<')
				state = 10;
			break;

		case 1:
			if (ch == ']')
				state = 2;
			else
			{
				retract();
				state = 0;
			}
			break;

		case 2:
			if (ch == '>')
				done = true;
			else if (ch != ']')
			{
				retract();
				state = 0;
			}
			break;

		case 10:
			if (ch == '!')
				state = 11;
			else
			{
				retract();
				state = 0;
			}
			break;

		case 11:
			if (ch == '[')
			{
				ignoresectcontents();
				state = 0;
			}
			else
			{
				retract();
				state = 0;
			}
			break;
		}
	}
}

void parser_imp::markup_decl()
{
	value_saver<bool> allow_parameter_entity_references(
		m_allow_parameter_entity_references, m_external_subset);

	switch (m_lookahead)
	{
	case xml_Element:
		element_decl();
		break;

	case xml_AttList:
		attlist_decl();
		break;

	case xml_Entity:
		entity_decl();
		break;

	case xml_Notation:
		notation_decl();
		break;

	case xml_PI:
		pi();
		break;

	case xml_Comment:
		comment();
		break;

	case xml_Space:
		s();
		break;
	}
}

void parser_imp::element_decl()
{
	valid_nesting_validator check(m_data_source);

	match(xml_Element);
	s(true);

	std::string name = m_token;

	auto e = std::find_if(m_doctype.begin(), m_doctype.end(),
						  [name](auto e) { return e->name() == name; });

	if (e == m_doctype.end())
		e = m_doctype.insert(m_doctype.end(), new doctype::element(name, true, m_in_external_dtd));
	else if ((*e)->declared())
		not_valid("duplicate element declaration for element '" + name + "'");
	else
		(*e)->external(m_in_external_dtd);

	match(xml_Name);
	s(true);

	contentspec(**e);
	s();

	m_allow_parameter_entity_references = true;

	check.check(m_data_source);
	match('>');
}

void parser_imp::contentspec(doctype::element& element)
{
	if (m_lookahead == xml_Name)
	{
		if (m_token == "EMPTY")
			element.set_allowed(new doctype::allowed_empty);
		else if (m_token == "ANY")
			element.set_allowed(new doctype::allowed_any);
		else
			not_well_formed("Invalid element content specification");
		match(xml_Name);
	}
	else
	{
		valid_nesting_validator check(m_data_source);
		match('(');

		std::unique_ptr<doctype::allowed_base> allowed;

		s();

		bool mixed = false;
		bool more = false;

		if (m_lookahead == '#') // Mixed
		{
			mixed = true;

			match(m_lookahead);
			if (m_token != "PCDATA")
				not_well_formed("Invalid element content specification, expected #PCDATA");
			match(xml_Name);

			s();

			std::set<std::string> seen;

			while (m_lookahead == '|')
			{
				more = true;

				match('|');
				s();

				if (seen.count(m_token) > 0)
					not_valid("no duplicates allowed in mixed content for element declaration");
				seen.insert(m_token);

				match(xml_Name);
				s();
			}

			doctype::allowed_choice *choice = new doctype::allowed_choice(true);
			for (const std::string& c : seen)
				choice->add(new doctype::allowed_element(c));
			allowed.reset(choice);
		}
		else // children
		{
			allowed.reset(cp());

			s();

			if (m_lookahead == ',')
			{
				doctype::allowed_seq *seq = new doctype::allowed_seq(allowed.release());
				allowed.reset(seq);

				more = true;
				do
				{
					match(m_lookahead);
					s();
					seq->add(cp());
					s();
				} while (m_lookahead == ',');
			}
			else if (m_lookahead == '|')
			{
				doctype::allowed_choice *choice = new doctype::allowed_choice(allowed.release(), false);
				allowed.reset(choice);

				more = true;
				do
				{
					match(m_lookahead);
					s();
					choice->add(cp());
					s();
				} while (m_lookahead == '|');
			}
		}

		s();

		check.check(m_data_source);
		match(')');

		if (m_lookahead == '*')
		{
			allowed.reset(new doctype::allowed_repeated(allowed.release(), '*'));
			match('*');
		}
		else if (more)
		{
			if (mixed)
			{
				allowed.reset(new doctype::allowed_repeated(allowed.release(), '*'));
				match('*');
			}
			else if (m_lookahead == '+')
			{
				allowed.reset(new doctype::allowed_repeated(allowed.release(), '+'));
				match('+');
			}
			else if (m_lookahead == '?')
			{
				allowed.reset(new doctype::allowed_repeated(allowed.release(), '?'));
				match('?');
			}
		}

		element.set_allowed(allowed.release());
	}
}

doctype::allowed_ptr parser_imp::cp()
{
	std::unique_ptr<doctype::allowed_base> result;

	if (m_lookahead == '(')
	{
		valid_nesting_validator check(m_data_source);

		match('(');

		s();
		result.reset(cp());
		s();
		if (m_lookahead == ',')
		{
			doctype::allowed_seq *seq = new doctype::allowed_seq(result.release());
			result.reset(seq);

			do
			{
				match(m_lookahead);
				s();
				seq->add(cp());
				s();
			} while (m_lookahead == ',');
		}
		else if (m_lookahead == '|')
		{
			doctype::allowed_choice *choice = new doctype::allowed_choice(result.release(), false);
			result.reset(choice);

			do
			{
				match(m_lookahead);
				s();
				choice->add(cp());
				s();
			} while (m_lookahead == '|');
		}

		s();
		check.check(m_data_source);
		match(')');
	}
	else
	{
		std::string name = m_token;
		match(xml_Name);

		result.reset(new doctype::allowed_element(name));
	}

	switch (m_lookahead)
	{
	case '*':
		result.reset(new doctype::allowed_repeated(result.release(), '*'));
		match('*');
		break;
	case '+':
		result.reset(new doctype::allowed_repeated(result.release(), '+'));
		match('+');
		break;
	case '?':
		result.reset(new doctype::allowed_repeated(result.release(), '?'));
		match('?');
		break;
	}

	return result.release();
}

void parser_imp::entity_decl()
{
	value_saver<bool> allow_parameter_entity_references(m_allow_parameter_entity_references, true);

	match(xml_Entity);
	s(true);

	if (m_lookahead == '%') // PEDecl
		parameter_entity_decl();
	else
		general_entity_decl();
}

void parser_imp::parameter_entity_decl()
{
	match('%');
	s(true);

	std::string name = m_token;
	match(xml_Name);

	s(true);

	std::string path;
	std::string value;

	m_allow_parameter_entity_references = false;

	// PEDef is either a EntityValue...
	if (m_lookahead == xml_String)
	{
		value = m_token;
		match(xml_String);
		parse_parameter_entity_declaration(value);
	}
	else // ... or an external id
	{
		std::tie(path, value) = read_external_id();
		match(xml_String);
	}

	s();

	m_allow_parameter_entity_references = true;
	match('>');

	if (find_if(m_parameter_entities.begin(), m_parameter_entities.end(),
				[name](auto e) { return e->name() == name; }) == m_parameter_entities.end())
	{
		m_parameter_entities.push_back(new doctype::parameter_entity(name, value, path));
	}
}

void parser_imp::general_entity_decl()
{
	std::string name = m_token;
	match(xml_Name);
	s(true);

	std::string value, ndata;
	bool external = false;
	bool parsed = true;

	if (m_lookahead == xml_String)
	{
		value = m_token;
		match(xml_String);

		parse_general_entity_declaration(value);
	}
	else // ... or an ExternalID
	{
		std::tie(std::ignore, value) = read_external_id();
		match(xml_String);
		external = true;

		if (m_lookahead == xml_Space)
		{
			s(true);
			if (m_lookahead == xml_Name and m_token == "NDATA")
			{
				match(xml_Name);
				s(true);

				parsed = false;
				ndata = m_token;

				match(xml_Name);
			}
		}
	}

	s();

	m_allow_parameter_entity_references = true;
	match('>');

	if (std::find_if(m_general_entities.begin(), m_general_entities.end(),
					 [name](auto e) { return e->name() == name; }) == m_general_entities.end())
	{
		m_general_entities.push_back(new doctype::general_entity(name, value, external, parsed));

		if (not parsed)
			m_general_entities.back()->ndata(ndata);

		if (m_in_external_dtd)
			m_general_entities.back()->externally_defined(true);
	}
}

void parser_imp::attlist_decl()
{
	match(xml_AttList);
	s(true);
	std::string element = m_token;
	match(xml_Name);

	auto dte = find_if(m_doctype.begin(), m_doctype.end(),
					   [element](auto e) { return e->name() == element; });

	if (dte == m_doctype.end())
		dte = m_doctype.insert(m_doctype.end(), new doctype::element(element, false, m_in_external_dtd));

	// attdef

	while (m_lookahead == xml_Space)
	{
		s(true);

		if (m_lookahead != xml_Name)
			break;

		std::string name = m_token;
		match(xml_Name);
		s(true);

		std::unique_ptr<doctype::attribute> attribute;

		// att type: several possibilities:
		if (m_lookahead == '(') // enumeration
		{
			std::vector<std::string> enums;

			match(m_lookahead);

			s();

			enums.push_back(m_token);
			if (m_lookahead == xml_Name)
				match(xml_Name);
			else
				match(xml_NMToken);

			s();

			while (m_lookahead == '|')
			{
				match('|');

				s();

				enums.push_back(m_token);
				if (m_lookahead == xml_Name)
					match(xml_Name);
				else
					match(xml_NMToken);

				s();
			}

			s();

			match(')');

			attribute.reset(new doctype::attribute(name, doctype::attTypeEnumerated, enums));
		}
		else
		{
			std::string type = m_token;
			match(xml_Name);

			std::vector<std::string> notations;

			if (type == "CDATA")
				attribute.reset(new doctype::attribute(name, doctype::attTypeString));
			else if (type == "ID")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedID));
			else if (type == "IDREF")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedIDREF));
			else if (type == "IDREFS")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedIDREFS));
			else if (type == "ENTITY")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedENTITY));
			else if (type == "ENTITIES")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedENTITIES));
			else if (type == "NMTOKEN")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedNMTOKEN));
			else if (type == "NMTOKENS")
				attribute.reset(new doctype::attribute(name, doctype::attTypeTokenizedNMTOKENS));
			else if (type == "NOTATION")
			{
				s(true);
				match('(');
				s();

				notations.push_back(m_token);
				match(xml_Name);

				s();

				while (m_lookahead == '|')
				{
					match('|');

					s();

					notations.push_back(m_token);
					match(xml_Name);

					s();
				}

				s();

				match(')');

				attribute.reset(new doctype::attribute(name, doctype::attTypeNotation, notations));
			}
			else
				not_well_formed("invalid attribute type");
		}

		// att def

		s(true);

		std::string value;

		if (m_lookahead == '#')
		{
			match(m_lookahead);
			std::string def = m_token;
			match(xml_Name);

			if (def == "REQUIRED")
				attribute->set_default(doctype::attDefRequired, "");
			else if (def == "IMPLIED")
				attribute->set_default(doctype::attDefImplied, "");
			else if (def == "FIXED")
			{
				if (attribute->get_type() == doctype::attTypeTokenizedID)
					not_valid("the default declaration for an ID attribute declaration should be #IMPLIED or #REQUIRED");

				s(true);

				std::string value = m_token;
				normalize_attribute_value(value);
				if (not value.empty() and not attribute->validate_value(value, m_general_entities))
				{
					not_valid("default value '" + value + "' for attribute '" + name + "' is not valid");
				}

				attribute->set_default(doctype::attDefFixed, value);
				match(xml_String);
			}
			else
				not_well_formed("invalid attribute default");
		}
		else
		{
			if (attribute->get_type() == doctype::attTypeTokenizedID)
				not_valid("the default declaration for an ID attribute declaration should be #IMPLIED or #REQUIRED");

			std::string value = m_token;
			normalize_attribute_value(value);
			if (not value.empty() and not attribute->validate_value(value, m_general_entities))
			{
				not_valid("default value '" + value + "' for attribute '" + name + "' is not valid");
			}
			attribute->set_default(doctype::attDefNone, value);
			match(xml_String);
		}

		if (attribute->get_type() == doctype::attTypeTokenizedID)
		{
			const doctype::attribute_list& atts = (*dte)->attributes();
			if (std::find_if(atts.begin(), atts.end(),
							 [](auto a) { return a->get_type() == doctype::attTypeTokenizedID; }) != atts.end())
				not_valid("only one attribute per element can have the ID type");
		}

		attribute->external(m_in_external_dtd);
		(*dte)->add_attribute(attribute.release());
	}

	m_allow_parameter_entity_references = true;
	match('>');
}

void parser_imp::notation_decl()
{
	match(xml_Notation);
	s(true);

	std::string name = m_token, pubid, sysid;

	if (m_notations.count(name) > 0)
		not_valid("notation names should be unique");
	m_notations.insert(name);

	match(xml_Name);
	s(true);

	if (m_token == "SYSTEM")
	{
		match(xml_Name);
		s(true);

		sysid = m_token;
		match(xml_String);

		if (not is_valid_system_literal(sysid))
			not_well_formed("invalid system literal");
	}
	else if (m_token == "PUBLIC")
	{
		match(xml_Name);
		s(true);

		pubid = m_token;
		match(xml_String);

		// validate the public ID
		if (not is_valid_public_id(pubid))
			not_well_formed("Invalid public ID");

		s();

		if (m_lookahead == xml_String)
		{
			sysid = m_token;
			match(xml_String);
		}
	}
	else
		not_well_formed("Expected either SYSTEM or PUBLIC");

	s();

	m_allow_parameter_entity_references = true;
	match('>');

	m_parser.notation_decl(name, sysid, pubid);
}

data_source *parser_imp::external_id()
{
	data_source *result = nullptr;
	std::string pubid, sysid;

	if (m_token == "SYSTEM")
	{
		match(xml_Name);
		s(true);

		sysid = m_token;

		if (not is_valid_system_literal(sysid))
			not_well_formed("invalid system literal");
	}
	else if (m_token == "PUBLIC")
	{
		match(xml_Name);
		s(true);

		pubid = m_token;
		match(xml_String);

		// validate the public ID
		if (not is_valid_public_id(pubid))
			not_well_formed("Invalid public ID");

		s(true);
		sysid = m_token;
	}
	else
		not_well_formed("Expected external id starting with either SYSTEM or PUBLIC");

	std::istream *is = m_parser.external_entity_ref(m_data_source->base(), pubid, sysid);
	if (is != nullptr)
	{
		result = new istream_data_source(is);

		std::string::size_type s = sysid.rfind('/');
		if (s == std::string::npos)
			result->base(m_data_source->base());
		else
		{
			sysid.erase(s, std::string::npos);

			if (is_absolute_path(sysid))
				result->base(sysid);
			else
				result->base(m_data_source->base() + '/' + sysid);
		}
	}

	return result;
}

std::tuple<std::string, std::string> parser_imp::read_external_id()
{
	std::string result;
	std::string path;

	std::unique_ptr<data_source> data(external_id());

	parser_state save(this, data.get());

	if (m_data_source)
	{
		path = m_data_source->base();

		m_lookahead = get_next_token();

		text_decl();

		result = m_token;

		while (unicode ch = get_next_char())
			append(result, ch);
	}

	return std::make_tuple(path, result);
}

void parser_imp::parse_parameter_entity_declaration(std::string& s)
{
	std::string result;

	int state = 0;
	unicode charref = 0;
	std::string name;

	for (std::string::const_iterator i = s.begin(); i != s.end(); ++i)
	{
		unicode c = *i;

		switch (state)
		{
		case 0:
			if (c == '&')
				state = 1;
			else if (c == '%')
			{
				if (m_external_subset)
				{
					name.clear();
					state = 20;
				}
				else
					not_well_formed("parameter entities may not occur in declarations that are not in an external subset");
			}
			else
				append(result, c);
			break;

		case 1:
			if (c == '#')
				state = 2;
			else
			{
				result += '&';
				append(result, c);
				state = 0;
			}
			break;

		case 2:
			if (c == 'x')
				state = 4;
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = 3;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 3:
			if (c >= '0' and c <= '9')
				charref = charref * 10 + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: " + to_hex(charref) + '\'');

				append(result, charref);
				state = 0;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 4:
			if (c >= 'a' and c <= 'f')
			{
				charref = c - 'a' + 10;
				state = 5;
			}
			else if (c >= 'A' and c <= 'F')
			{
				charref = c - 'A' + 10;
				state = 5;
			}
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = 5;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 5:
			if (c >= 'a' and c <= 'f')
				charref = (charref << 4) + (c - 'a' + 10);
			else if (c >= 'A' and c <= 'F')
				charref = (charref << 4) + (c - 'A' + 10);
			else if (c >= '0' and c <= '9')
				charref = (charref << 4) + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: '" + to_hex(charref) + '\'');

				append(result, charref);
				state = 0;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 20:
			if (c == ';')
			{
				const doctype::entity& e = get_parameter_entity(name);
				result += e.replacement();
				state = 0;
			}
			else if (is_name_char(c))
				append(name, c);
			else
				not_well_formed("invalid parameter entity reference");
			break;

		default:
			assert(false);
			not_well_formed("invalid state");
		}
	}

	if (state != 0)
		not_well_formed("invalid reference");

	swap(s, result);
}

// parse out the general and parameter entity references in a value std::string
// for a general entity reference which is about to be stored.
void parser_imp::parse_general_entity_declaration(std::string& s)
{
	std::string result;

	int state = 0;
	unicode charref = 0;
	std::string name;

	for (std::string::const_iterator i = s.begin(); i != s.end(); ++i)
	{
		unicode c = *i;

		switch (state)
		{
		case 0:
			if (c == '&')
				state = 1;
			else if (c == '%')
			{
				if (m_external_subset)
				{
					name.clear();
					state = 20;
				}
				else
					not_well_formed("parameter entities may not occur in declarations that are not in an external subset");
			}
			else
				append(result, c);
			break;

		case 1:
			if (c == '#')
				state = 2;
			else if (is_name_start_char(c))
			{
				name.clear();
				append(name, c);
				state = 10;
			}
			break;

		case 2:
			if (c == 'x')
				state = 4;
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = 3;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 3:
			if (c >= '0' and c <= '9')
				charref = charref * 10 + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: '" + to_hex(charref) + '\'');

				append(result, charref);
				state = 0;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 4:
			if (c >= 'a' and c <= 'f')
			{
				charref = c - 'a' + 10;
				state = 5;
			}
			else if (c >= 'A' and c <= 'F')
			{
				charref = c - 'A' + 10;
				state = 5;
			}
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = 5;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 5:
			if (c >= 'a' and c <= 'f')
				charref = (charref << 4) + (c - 'a' + 10);
			else if (c >= 'A' and c <= 'F')
				charref = (charref << 4) + (c - 'A' + 10);
			else if (c >= '0' and c <= '9')
				charref = (charref << 4) + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: '" + to_hex(charref) + '\'');

				append(result, charref);
				state = 0;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case 10:
			if (c == ';')
			{
				result += '&';
				result += name;
				result += ';';

				state = 0;
			}
			else if (is_name_char(c))
				append(name, c);
			else
				not_well_formed("invalid entity reference");
			break;

		case 20:
			if (c == ';')
			{
				const doctype::entity& e = get_parameter_entity(name);
				result += e.replacement();
				state = 0;
			}
			else if (is_name_char(c))
				append(name, c);
			else
				not_well_formed("invalid parameter entity reference");
			break;

		default:
			assert(false);
			not_well_formed("invalid state");
		}
	}

	if (state != 0)
		not_well_formed("invalid reference");

	swap(s, result);
}

std::string parser_imp::normalize_attribute_value(data_source *data)
{
	std::string result;

	unicode charref = 0;
	std::string name;

	enum State
	{
		state_Start,
		state_ReferenceStart,
		state_CharReferenceStart,
		state_HexCharReference,
		state_HexCharReference2,
		state_DecCharReference,
		state_EntityReference,

	} state = state_Start;

	for (;;)
	{
		unicode c = data->get_next_char();

		if (c == 0)
			break;

		if (c == '<')
			not_well_formed("Attribute values may not contain '<' character");

		switch (state)
		{
		case state_Start:
			if (c == '&')
				state = state_ReferenceStart;
			else if (c == ' ' or c == '\n' or c == '\t' or c == '\r')
				result += ' ';
			else
				append(result, c);
			break;

		case state_ReferenceStart:
			if (c == '#')
				state = state_CharReferenceStart;
			else if (is_name_start_char(c))
			{
				name.clear();
				append(name, c);
				state = state_EntityReference;
			}
			else
				not_well_formed("invalid reference found in attribute value");
			break;

		case state_CharReferenceStart:
			if (c == 'x')
				state = state_HexCharReference;
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = state_DecCharReference;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_DecCharReference:
			if (c >= '0' and c <= '9')
				charref = charref * 10 + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: '" + to_hex(charref) + '\'');

				append(result, charref);
				state = state_Start;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_HexCharReference:
			if (c >= 'a' and c <= 'f')
			{
				charref = c - 'a' + 10;
				state = state_HexCharReference2;
			}
			else if (c >= 'A' and c <= 'F')
			{
				charref = c - 'A' + 10;
				state = state_HexCharReference2;
			}
			else if (c >= '0' and c <= '9')
			{
				charref = c - '0';
				state = state_HexCharReference2;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_HexCharReference2:
			if (c >= 'a' and c <= 'f')
				charref = (charref << 4) + (c - 'a' + 10);
			else if (c >= 'A' and c <= 'F')
				charref = (charref << 4) + (c - 'A' + 10);
			else if (c >= '0' and c <= '9')
				charref = (charref << 4) + (c - '0');
			else if (c == ';')
			{
				if (not is_char(charref))
					not_well_formed("Illegal character referenced: '" + to_hex(charref) + '\'');

				append(result, charref);
				state = state_Start;
			}
			else
				not_well_formed("invalid character reference");
			break;

		case state_EntityReference:
			if (c == ';')
			{
				if (data->is_entity_on_stack(name))
					not_well_formed("infinite recursion in nested entity references");

				const doctype::entity& e = get_general_entity(name);

				if (e.external())
					not_well_formed("attribute value may not contain external entity reference");

				if (e.externally_defined() and m_standalone)
					not_well_formed("document marked as standalone but an external entity is referenced");

				entity_data_source next_data(name, m_data_source->base(), e.replacement(), data);
				std::string replacement = normalize_attribute_value(&next_data);
				result += replacement;

				state = state_Start;
			}
			else if (is_name_char(c))
				append(name, c);
			else
				not_well_formed("invalid entity reference");
			break;

		default:
			assert(false);
			not_well_formed("invalid state");
		}
	}

	if (state != state_Start)
		not_well_formed("invalid reference");

	return result;
}

void parser_imp::element(doctype::validator& valid)
{
	value_saver<bool> in_element(m_in_element, true);
	value_saver<bool> in_content(m_in_content, false);

	match(xml_STag);
	std::string name = m_token;
	match(xml_Name);

	if (not valid(name))
		not_valid("element '" + name + "' not expected at this position");

	const doctype::element *dte = get_element(name);

	if (m_has_dtd and dte == nullptr and m_validating)
		not_valid("Element '" + name + "' is not defined in DTD");

	doctype::validator sub_valid;
	if (dte != nullptr)
		sub_valid = dte->get_validator();

	std::list<detail::attr> attrs;

	ns_state ns(this);
	std::set<std::string> seen;

	for (;;)
	{
		if (m_lookahead != xml_Space)
			break;

		s(true);

		if (m_lookahead != xml_Name)
			break;

		std::string attr_name = m_token;
		match(xml_Name);

		if (seen.count(attr_name) > 0)
			not_well_formed("multiple values for attribute '" + attr_name + "'");
		seen.insert(attr_name);

		eq();

		std::string attr_value = normalize_attribute_value(m_token);
		match(xml_String);

		const doctype::attribute *dta = nullptr;
		if (dte != nullptr)
			dta = dte->get_attribute(attr_name);

		if (dta == nullptr and m_validating)
			not_valid("undeclared attribute '" + attr_name + "'");

		if (m_validating and
			dta != nullptr and
			dta->get_default_type() == doctype::attDefFixed and
			attr_value != std::get<1>(dta->get_default()))
		{
			not_valid("invalid value specified for fixed attribute");
		}

		// had a crash suddenly here deep down in ba::starts_with...
		if (attr_name == "xmlns" or attr_name.compare(0, 6, "xmlns:", 6) == 0) // namespace support
		{
			if (attr_name.length() == 5)
			{
				ns.m_default_ns = attr_value;
				m_parser.start_namespace_decl("", attr_value);
			}
			else
			{
				std::string prefix = attr_name.substr(6);
				ns.m_known[prefix] = attr_value;
				m_parser.start_namespace_decl(prefix, attr_value);
			}
		}
		else
		{
			bool id = (attr_name == "xml:id");

			if (dta != nullptr)
			{
				std::string v(attr_value);

				if (not dta->validate_value(attr_value, m_general_entities))
				{
					not_valid("invalid value ('" + attr_value + "') for attribute " + attr_name + "");
				}

				if (m_validating and m_standalone and dta->external() and v != attr_value)
					not_valid("attribute value modified as a result of an external defined attlist declaration, which is not valid in a standalone document");

				if (dta->get_type() == doctype::attTypeTokenizedID)
				{
					id = true;

					if (m_ids.count(attr_value) > 0)
					{
						not_valid("attribute value ('" + attr_value + "') for attribute '" + attr_name + "' is not unique");
					}

					m_ids.insert(attr_value);

					if (m_unresolved_ids.count(attr_value) > 0)
						m_unresolved_ids.erase(attr_value);
				}
				else if (dta->get_type() == doctype::attTypeTokenizedIDREF)
				{
					if (attr_value.empty())
						not_valid("attribute value for attribute '" + attr_name + "' may not be empty");

					if (not m_ids.count(attr_value))
						m_unresolved_ids.insert(attr_value);
				}
				else if (dta->get_type() == doctype::attTypeTokenizedIDREFS)
				{
					if (attr_value.empty())
						not_valid("attribute value for attribute '" + attr_name + "' may not be empty");

					std::string::size_type b = 0, e = attr_value.find(' ');
					while (e != std::string::npos)
					{
						if (e - b > 0)
						{
							std::string id = attr_value.substr(b, e);
							if (not m_ids.count(id))
								m_unresolved_ids.insert(id);
						}
						b = e + 1;
						e = attr_value.find(' ', b);
					}

					if (b != std::string::npos and b < attr_value.length())
					{
						std::string id = attr_value.substr(b);
						if (not m_ids.count(id))
							m_unresolved_ids.insert(id);
					}
				}
			}

			detail::attr attr;
			attr.m_name = attr_name;
			attr.m_value = attr_value;
			attr.m_id = id;

			if (m_ns != nullptr)
			{
				std::string::size_type d = attr_name.find(':');
				if (d != std::string::npos)
				{
					std::string ns = m_ns->ns_for_prefix(attr_name.substr(0, d));

					if (not ns.empty())
					{
						attr.m_ns = ns;
						attr.m_name = attr_name.substr(d + 1);
					}
				}
			}

			attrs.push_back(attr);
		}
	}

	// add missing attributes
	if (dte != nullptr)
	{
		for (const doctype::attribute *dta : dte->attributes())
		{
			std::string attr_name = dta->name();

			std::list<detail::attr>::iterator attr = find_if(attrs.begin(), attrs.end(),
															 [attr_name](auto& a) { return a.m_name == attr_name; });

			doctype::AttributeDefault defType;
			std::string defValue;

			std::tie(defType, defValue) = dta->get_default();

			if (defType == doctype::attDefRequired)
			{
				if (attr == attrs.end())
					not_valid("missing #REQUIRED attribute '" + attr_name + "' for element '" + name + "'");
			}
			else if (not defValue.empty() and attr == attrs.end())
			{
				if (m_validating and m_standalone and dta->external())
					not_valid("default value for attribute defined in external declaration which is not allowed in a standalone document");

				detail::attr attr;
				attr.m_name = attr_name;
				attr.m_value = normalize_attribute_value(defValue);
				attr.m_id = false;

				if (m_ns != nullptr)
				{
					std::string::size_type d = attr_name.find(':');
					if (d != std::string::npos)
					{
						std::string ns = m_ns->ns_for_prefix(attr_name.substr(0, d));

						if (not ns.empty())
						{
							attr.m_ns = ns;
							attr.m_name = attr_name.substr(d + 1);
						}
					}
				}

				attrs.push_back(attr);
			}
		}
	}

	// now find out the namespace we're supposed to pass
	std::string uri, raw(name);

	std::string::size_type c = name.find(':');
	if (c != std::string::npos and c > 0)
	{
		uri = ns.ns_for_prefix(name.substr(0, c));
		name.erase(0, c + 1);
	}
	else
		uri = ns.default_ns();

	// sort the attributes (why? disabled to allow similar output)
	// attrs.sort([](auto& a, auto& b) { return a.m_name < b.m_name; });

	if (m_lookahead == '/')
	{
		match('/');
		m_parser.start_element(name, uri, attrs);
		m_parser.end_element(name, uri);
	}
	else
	{
		m_parser.start_element(name, uri, attrs);

		// open scope, we're entering a content production
		{
			value_saver<bool> save(m_in_content, true);
			match('>');

			if (m_lookahead != xml_ETag)
				content(sub_valid, m_validating and m_standalone and dte->external() and dte->element_content());
		}

		match(xml_ETag);

		if (m_token != raw)
			not_well_formed("end tag does not match start tag");

		match(xml_Name);

		s();

		m_parser.end_element(name, uri);
	}

	m_in_content = in_content.m_value;
	match('>');

	if (m_validating and dte != nullptr and not sub_valid.done())
		not_valid("missing child elements for element '" + dte->name() + "'");

	s();
}

void parser_imp::content(doctype::validator& valid, bool check_for_whitespace)
{
	do
	{
		switch (m_lookahead)
		{
		case xml_Content:
			if (valid.allow_char_data())
				m_parser.character_data(m_token);
			else
			{
				ba::trim(m_token);
				if (m_token.empty())
				{
					if (check_for_whitespace)
						not_valid("element declared in external subset contains white space");
				}
				else
					not_valid("character data '" + m_token + "' not allowed in element");
			}
			match(xml_Content);
			break;

		case xml_Reference:
		{
			const doctype::entity& e = get_general_entity(m_token);

			if (m_data_source->is_entity_on_stack(m_token))
				not_well_formed("infinite recursion of entity references");

			if (e.externally_defined() and m_standalone)
				not_well_formed("document marked as standalone but an external entity is referenced");

			if (not e.parsed())
				not_well_formed("content has a general entity reference to an unparsed entity");

			// scope
			{
				entity_data_source source(m_token, m_data_source->base(), e.replacement(), m_data_source);
				parser_state state(this, &source);

				m_lookahead = get_next_content();
				m_in_external_dtd = e.externally_defined();

				if (m_lookahead != xml_Eof)
					content(valid, check_for_whitespace);

				if (m_lookahead != xml_Eof)
					not_well_formed("entity reference should be a valid content production");
			}

			match(xml_Reference);
			break;
		}

		case xml_STag:
			element(valid);
			break;

		case xml_PI:
			pi();
			break;

		case xml_Comment:
			comment();
			break;

		case xml_Space:
			s();
			break;

		case xml_CDSect:
			if (not valid.allow_char_data())
				not_valid("character data '" + m_token + "' not allowed in element");

			m_parser.start_cdata_section();
			m_parser.character_data(m_token);
			m_parser.end_cdata_section();

			match(xml_CDSect);
			break;

		default:
			match(xml_Content); // will fail and report error
		}
	} while (m_lookahead != xml_ETag and m_lookahead != xml_Eof);
}

void parser_imp::comment()
{
	// m_lookahead == xml_Comment
	// read characters until we reach -->
	// check all characters in between for validity

	enum
	{
		state_Start,
		state_FirstHyphenSeen,
		state_SecondHyphenSeen,
		state_CommentClosed
	} state = state_Start;

	m_token.clear();

	while (state != state_CommentClosed)
	{
		unicode ch = get_next_char();

		if (ch == 0)
			not_well_formed("runaway comment");
		if (not is_char(ch))
			not_well_formed("illegal character in content: '" + to_hex(ch) + '\'');

		switch (state)
		{
		case state_Start:
			if (ch == '-')
				state = state_FirstHyphenSeen;
			break;

		case state_FirstHyphenSeen:
			if (ch == '-')
				state = state_SecondHyphenSeen;
			else
				state = state_Start;
			break;

		case state_SecondHyphenSeen:
			if (ch == '>')
				state = state_CommentClosed;
			else
				not_well_formed("double hyphen found in comment");
			break;

		case state_CommentClosed:
			assert(false);
		}
	}

	assert(m_token.length() >= 3);
	m_token.erase(m_token.end() - 3, m_token.end());
	m_parser.comment(m_token);

	match(xml_Comment);
}

void parser_imp::pi()
{
	// m_lookahead == xml_PI
	// read characters until we reach -->
	// check all characters in between for validity

	std::string pi_target = m_token.substr(2);

	if (pi_target.empty())
		not_well_formed("processing instruction target missing");

	// we treat the xml processing instruction separately.
	if (m_token.substr(2) == "xml")
		not_well_formed("xml declaration are only valid as the start of the file");
	else if (iequals(pi_target, "xml"))
		not_well_formed("<?XML is neither an XML declaration nor a legal processing instruction target");

	enum
	{
		state_Start,
		state_DataStart,
		state_Data,
		state_QuestionMarkSeen,
		state_PIClosed
	} state = state_Start;

	m_token.clear();

	while (state != state_PIClosed)
	{
		unicode ch = get_next_char();

		if (ch == 0)
			not_well_formed("runaway processing instruction");
		if (not is_char(ch))
			not_well_formed("illegal character in processing instruction: '" + to_hex(ch) + '\'');

		switch (state)
		{
		case state_Start:
			if (ch == '?')
				state = state_QuestionMarkSeen;
			else if (ch == ' ' or ch == '\n' or ch == '\t')
			{
				m_token.clear();
				state = state_DataStart;
			}
			else
				not_well_formed("a space is required before pi data");
			break;

		case state_DataStart:
			if (ch == ' ' or ch == '\n' or ch == '\t')
				m_token.clear();
			else if (ch == '?')
				state = state_QuestionMarkSeen;
			else
				state = state_Data;
			break;

		case state_Data:
			if (ch == '?')
				state = state_QuestionMarkSeen;
			break;

		case state_QuestionMarkSeen:
			if (ch == '>')
				state = state_PIClosed;
			else if (ch != '?')
				state = state_Data;
			break;

		case state_PIClosed:
			assert(false);
		}
	}

	m_token.erase(m_token.end() - 2, m_token.end());
	m_parser.processing_instruction(pi_target, m_token);

	match(xml_PI);
}

// --------------------------------------------------------------------

parser::parser(std::istream& data)
	: m_impl(new parser_imp(data, *this)), m_istream(nullptr)
{
}

parser::parser(const std::string& data)
{
	m_istream = new std::istringstream(data);
	m_impl = new parser_imp(*m_istream, *this);
}

parser::~parser()
{
	delete m_impl;
	delete m_istream;
}

void parser::parse(bool validate)
{
	m_impl->parse(validate);
}

void parser::start_element(const std::string& name, const std::string& uri, const std::list<detail::attr> &atts)
{
	if (start_element_handler)
		start_element_handler(name, uri, atts);
}

void parser::end_element(const std::string& name, const std::string& uri)
{
	if (end_element_handler)
		end_element_handler(name, uri);
}

void parser::character_data(const std::string& data)
{
	if (character_data_handler)
		character_data_handler(data);
}

void parser::processing_instruction(const std::string& target, const std::string& data)
{
	if (processing_instruction_handler)
		processing_instruction_handler(target, data);
}

void parser::comment(const std::string& data)
{
	if (comment_handler)
		comment_handler(data);
}

void parser::start_cdata_section()
{
	if (start_cdata_section_handler)
		start_cdata_section_handler();
}

void parser::end_cdata_section()
{
	if (end_cdata_section_handler)
		end_cdata_section_handler();
}

void parser::start_namespace_decl(const std::string& prefix, const std::string& uri)
{
	if (start_namespace_decl_handler)
		start_namespace_decl_handler(prefix, uri);
}

void parser::end_namespace_decl(const std::string& prefix)
{
	if (end_namespace_decl_handler)
		end_namespace_decl_handler(prefix);
}

void parser::notation_decl(const std::string& name, const std::string& systemId, const std::string& publicId)
{
	if (notation_decl_handler)
		notation_decl_handler(name, systemId, publicId);
}

std::istream *parser::external_entity_ref(const std::string& base, const std::string& pubid, const std::string& uri)
{
	std::istream *result = nullptr;
	if (external_entity_ref_handler)
		result = external_entity_ref_handler(base, pubid, uri);
	return result;
}

void parser::report_invalidation(const std::string& msg)
{
	if (report_invalidation_handler)
		report_invalidation_handler(msg);
}

} // namespace xml
} // namespace zeep