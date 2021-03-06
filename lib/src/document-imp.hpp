// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
//  Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#ifndef SOAP_XML_DOCUMENT_IMP_H
#define SOAP_XML_DOCUMENT_IMP_H

#include <iostream>

#include <zeep/xml/document.hpp>

namespace zeep
{
namespace xml
{

struct document_imp
{
	document_imp(document* doc);
	virtual ~document_imp();

	virtual void parse(std::istream& data) = 0;

	std::string prefix_for_namespace(const std::string& ns);

	root_node m_root;
	std::string m_dtd_dir;

	// some content information
	bool m_has_xml_decl;
	encoding_type m_encoding;
	float m_version;
	bool m_standalone;
	int m_indent;
	bool m_empty;
	bool m_wrap;
	bool m_trim;
	bool m_escape_whitespace;
	bool m_no_comment;

	bool m_validating;
	bool m_preserve_cdata;

	doc_type m_doctype;

	std::istream* external_entity_ref(const std::string& base,
									  const std::string& pubid,
									  const std::string& sysid);

	struct notation
	{
		std::string m_name;
		std::string m_sysid;
		std::string m_pubid;
	};

	document* m_doc;
	element* m_cur; // construction
	cdata* m_cdata; // only defined in a CDATA section
	std::vector<std::pair<std::string, std::string>> m_namespaces;
	std::list<notation> m_notations;
	size_t m_root_size_at_first_notation = 0;	// for processing instructions that occur before a notation
};

} // namespace xml
} // namespace zeep

#endif
