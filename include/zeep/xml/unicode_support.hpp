// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef ZEEP_XML_UNICODE_SUPPORT_HPP
#define ZEEP_XML_UNICODE_SUPPORT_HPP

#include <zeep/config.hpp>

#include <string>
#include <tuple>

namespace zeep { namespace xml {

/// We use our own unicode type since wchar_t might be too small.
/// This type should be able to contain a UCS4 encoded character.
typedef uint32_t unicode;

/// the supported encodings. Perhaps we should extend this list a bit?
enum class encoding_type
{
	enc_ASCII,
	enc_UTF8,			///< UTF-8
	enc_UTF16BE,		///< UTF-16 Big Endian
	enc_UTF16LE,		///< UTF 16 Little Endian
	enc_ISO88591
};

constexpr bool is_single_byte_encoding(encoding_type enc)
{
	return enc == encoding_type::enc_ASCII or enc == encoding_type::enc_ISO88591 or enc == encoding_type::enc_UTF8;
}

/// some character classification routines

bool is_name_start_char(unicode uc);
bool is_name_char(unicode uc);
bool is_valid_xml_1_0_char(unicode uc);
bool is_valid_xml_1_1_char(unicode uc);
bool is_valid_system_literal_char(unicode uc);
bool is_valid_system_literal(const std::string& s);
bool is_valid_public_id_char(unicode uc);
bool is_valid_public_id(const std::string& s);

/// Convert a string from UCS4 to UTF-8
std::string wstring_to_string(const std::wstring& s);

/// manipulate UTF-8 encoded strings
void append(std::string& s, unicode ch);
unicode pop_last_char(std::string& s);
template<typename Iter>
std::tuple<unicode,Iter> get_first_char(Iter ptr);

/// our own implementation of iequals: strings are compared case-insensitive
bool iequals(const std::string& a, const std::string& b);

/// Maybe not the correct location, but here's a to_string equivalent returning a hexadecimal representation.
std::string to_hex(int i);

// inlines

inline void append(std::string& s, unicode uc)
{
	if (uc < 0x080)
		s += (static_cast<char>(uc));
	else if (uc < 0x0800)
	{
		char ch[2] = {
			static_cast<char>(0x0c0 | (uc >> 6)),
			static_cast<char>(0x080 | (uc & 0x3f))
		};
		s.append(ch, 2);
	}
	else if (uc < 0x00010000)
	{
		char ch[3] = {
			static_cast<char>(0x0e0 | (uc >> 12)),
			static_cast<char>(0x080 | ((uc >> 6) & 0x3f)),
			static_cast<char>(0x080 | (uc & 0x3f))
		};
		s.append(ch, 3);
	}
	else
	{
		char ch[4] = {
			static_cast<char>(0x0f0 | (uc >> 18)),
			static_cast<char>(0x080 | ((uc >> 12) & 0x3f)),
			static_cast<char>(0x080 | ((uc >> 6) & 0x3f)),
			static_cast<char>(0x080 | (uc & 0x3f))
		};
		s.append(ch, 4);
	}
}

inline unicode pop_last_char(std::string& s)
{
	unicode result = 0;

	if (not s.empty())
	{
		std::string::iterator ch = s.end() - 1;
		
		if ((*ch & 0x0080) == 0)
		{
			result = *ch;
			s.erase(ch);
		}
		else
		{
			int o = 0;
			
			do
			{
				result |= (*ch & 0x03F) << o;
				o += 6;
				--ch;
			}
			while (ch != s.begin() and (*ch & 0x0C0) == 0x080);
			
			switch (o)
			{
				case  6: result |= (*ch & 0x01F) <<  6; break;
				case 12: result |= (*ch & 0x00F) << 12; break;
				case 18: result |= (*ch & 0x007) << 18; break;
			}
			
			s.erase(ch, s.end());
		}
	}
	
	return result;
}

// this code only works if the input is valid utf-8
template<typename Iter>
std::tuple<unicode,Iter> get_first_char(Iter ptr)
{
	unicode result = static_cast<unsigned char>(*ptr);
	++ptr;

	if (result > 0x07f)
	{
		unsigned char ch[3];
		
		if ((result & 0x0E0) == 0x0C0)
		{
			ch[0] = static_cast<unsigned char>(*ptr); ++ptr;
			result = ((result & 0x01F) << 6) | (ch[0] & 0x03F);
		}
		else if ((result & 0x0F0) == 0x0E0)
		{
			ch[0] = static_cast<unsigned char>(*ptr); ++ptr;
			ch[1] = static_cast<unsigned char>(*ptr); ++ptr;
			result = ((result & 0x00F) << 12) | ((ch[0] & 0x03F) << 6) | (ch[1] & 0x03F);
		}
		else if ((result & 0x0F8) == 0x0F0)
		{
			ch[0] = static_cast<unsigned char>(*ptr); ++ptr;
			ch[1] = static_cast<unsigned char>(*ptr); ++ptr;
			ch[2] = static_cast<unsigned char>(*ptr); ++ptr;
			result = ((result & 0x007) << 18) | ((ch[0] & 0x03F) << 12) | ((ch[1] & 0x03F) << 6) | (ch[2] & 0x03F);
		}
	}

	return std::make_tuple(result, ptr);
}

}
}

#endif
