/*
* This file is part of Wakanda software, licensed by 4D under
*  (i) the GNU General Public License version 3 (GNU GPL v3), or
*  (ii) the Affero General Public License version 3 (AGPL v3) or
*  (iii) a commercial license.
* This file remains the exclusive property of 4D and/or its licensors
* and is protected by national and international legislations.
* In any event, Licensee's compliance with the terms and conditions
* of the applicable license constitutes a prerequisite to any use of this file.
* Except as otherwise expressly stated in the applicable license,
* such license does not include any other license or rights on this file,
* 4D's and/or its licensors' trademarks and/or other proprietary rights.
* Consequently, no title, copyright or other proprietary rights
* other than those specified in the applicable license is granted.
*/
// Email library.
//
// Reference:
//
//		http://www.ietf.org/rfc/rfc5322.txt
//
// Usage:
//
// 		An email message is made up of a header and a body. The header is a set of fields (such as "From", "To", etc),
//		some of which may appear several times. Only the "Date" and "From" fields are mandatory. The body is just lines
//	 	terminated by CRLF sequences. CR and LF characters cannot appear alone inside body, they must alwats be in a
//		CRLF sequence.
//
//		It is possible to define and retrieve fields by using the bracket [] or dot . syntaxes. For instance, 
//
//			var myMessage = new Mail();
//
//			myMessage.Subject = 'this is an email';
//			myMessage['to'] = 'somebody@somewhere.com';
//
//		As some fields (such as "Comments") may appear an unlimited number of times, the value of a field can be an 
//		array of strings. It is preferable to use addField(), removeField(), and getField() functions instead, because 
//		they provide error checking (field's name syntax correctness and value as a string), also multiple fields are
//		handled automatically. Note that it is impossible to define field names having the same names as object's
//		methods. Currently, there is no check for syntax correctness of the assigned value. In particular, if values 
//		result in long lines, they must be folded accordingly (see section 2.2.3 of specification).
//
//		The content of a mail must be formatted according to section 2.3 of specification. The Mail object always 
//		stores properly formatted bodies. You can use the setBody() and getBody() to set or retrieve formatted bodies.
//		Use the setContent() and getContent() to set "unformatted" bodies, these functions will do the appropriate 
//		conversions.
//
//		Function getHeader() and getBody() return array of lines ready for sending by SMTP, just need to add CRLF at 
//		the end of each line. parse() function allows to read mail from POP3 or IMAP responses.

function MailScope () {
	
	// Maximum line length, see section 2.1.1 of specification.
	
	var	MAXIMUM_LINE_LENGTH			= 998;
	var RECOMMENDED_LINE_LIMIT		= 78;
	
	// Field name syntax, see section 2.2 of specification. Use fieldNameRegExp to check that a string has a valid
	// syntax for a field name. Use fieldNameDefinitionRegExp to match field name description (must start at the
	// beginning of line).
		
	var fieldNameRegExp				= /^[\x21-\x39\x3b-\x7e]+$/;
	var fieldNameDefinitionRegExp	= /^[\x21-\x39\x3b-\x7e]+:/;
	
	// Folded lines (in header) must start with a space or a tabulation (see section 2.2.3 of specification).
	
	var foldedLineStartRegExp		= /^ |\t/;
	
	function Mail (from, to, subject, content) {

		// Body must be properly formatted as explained in section 2.3 of specification.

		var	body	= new Array();	// An empty body is valid.
		
		// Exceptions for Mail, this will explain error, see codes below.
		
		var MailException = function (code) {
		
			this.code = code;
		
		} 
		MailException.INVALID_ARGUMENT	= -1;	// Function has been called with incorrect argument.
		MailException.INVALID_STATE		= -2;	// Should be impossible.
				
		// Add a field definition. It is recommended to use this function, because if the field is already defined,
		// it will be changed to an array of values. When you directly set a field using the bracket [] or dot . 
		// syntaxes, there is no check, and previous value (if any) is just overwritten. Note that some fields must
		// appear only once at most, there is (currently) no check for that. The value isn't check for syntax 
		// correctness.
		
		this.addField = function (name, value) {
		
			if (typeof name != 'string' || name.match(fieldNameRegExp) == null || typeof value != 'string') 
			
				throw new MailException(MailException.INVALID_ARGUMENT);
			
			else if (typeof this[name] != 'undefined') {
			
				if (this[name] instanceof Array) 
				
					this[name].push(value);
					
				else {
				
					var t;
					
					t = this[name];
					this[name] = new Array();
					this[name].push(t);
					this[name].push(value);
				
				}
			
			} else
			
				this[name] = value;
		
		}
		
		// Remove a field definition, does nothing if field with given name has not been defined. Second argument value
		// is optional, if given, this will remove field if the value matches. If the field definition is an array, 
		// this will only remove the given value from it.
		
		this.removeField = function (name, value) {
		
			if (typeof name != 'string' || name.match(fieldNameRegExp) == null 
			|| (arguments.length != 1 && typeof value != 'string'))
			
				throw new MailException(MailException.INVALID_ARGUMENT);
				
			else if (arguments.length == 1)
				
				delete this[name];

			else if (typeof this[name] == 'string') {
			
				if (this[name] == value)
				
					delete this[name];			
			
			} else if (this[name] instanceof Array) {
			
				for (k in this[name])
				
					if (this[name][k] == 'value') {
					
						delete this[name][k];
						break;
				
					}
			
			} else {
			
				// Do nothing. A correct field name is also a correct object member name, don't delete.
			
			}

		}
		
		// Return the value of field. Note that an array of values may be returned if field has been
		// defined several times.
		
		this.getField = function (name) {

			if (typeof name != 'string' || name.match(fieldNameRegExp) == null) 
			
				throw new MailException(MailException.INVALID_ARGUMENT);
		
			else
			
				return this[name];
		
		}

		// Return an array of lines, which is the header, just need to add CRLF at the end of each line to send 
		// using SMTP. There is (currently) no error checking: mandatory fields "From" and "Date" are not checked,
		// some fields may appear only a limited number of times, long lines must be folded properly. Note that 
		// an individual line may have CRLF sequence inside it because of folding.
		
		this.getHeader = function () {
		
			var	header = new Array();
			var name;
		
			for (name in this)
			
				if (name.match(fieldNameRegExp) == null)
					
					continue;
					
				else if (typeof this[name] == 'string')
				
					header.push(name + ': ' + this[name]);
							
				else if (this[name] instanceof Array) {
				
					var	i;
				
					for (i = 0; i < this[name].length; i++)
					
						if (typeof this[name][i] != 'string') 
						
							throw new MailException(MailException.INVALID_STATE);	// Impossible!
							
						else 
					
							header.push(name + ': ' + this[name][i]);
				
				} else 
				
					continue;	// Field names may "collide" with object's attributes, ignore.

			return header;
		
		}
		
		// Set the body of mail. The body must be a string or an array of lines (with no CRLF at end), correctly 
		// formatted according to section 2.3 of specification (no check for that). 

		this.setBody = function (newBody) {
		
			if (typeof newBody == 'string') {
			
				body = new Array();
				body.push(newBody);

			} else if (newBody instanceof Array) 
			
				body = newBody;
			
			else
			
				throw new MailException(MailException.INVALID_ARGUMENT);

		}
		
		// Retrieve body of mail, just ad CRLF at end of each line and it is ready to send using SMTP.

		this.getBody = function () {
		
			return body;
		
		}
		
		// Format and set the body of mail message, return true if successful. The new body can be a single 
		// string or an array of lines (strings). For an array of strings, the new body is considered as the
		// concatenation of all its strings. Formating replaces single '\r' (CR) characters by blanks and 
		// single '\n' (LF) characters by CRLF sequences. Lines' lengths are also checked. If too long, 
		// function returns false and current body is left unchanged. This function is slow, it is best to 
		// produce properly formatted body and use setBody() instead.
		
		this.setContent = function (content, lineLimit) {
		
			if (arguments > 1 && typeof lineLimit != 'number')
			
				throw new MailException(MailException.INVALID_ARGUMENT);
			
			if (typeof lineLimit != 'number')
				
				lineLimit = MAXIMUM_LINE_LENGTH;
		
			if (typeof content == 'string') {
			
				var	t;
				
				t = content;
				content = new Array();
				content.push(t);

			} else if (!(content instanceof Array))
			
				throw new MailException(MailException.INVALID_ARGUMENT);
							
			var formattedBody	= new Array();
			var i, j;
			
			for (i = 0, j = 0; i < content.length; i++) {
			
				if (typeof content[i] != 'string')
				
					throw new MailException(MailException.INVALID_ARGUMENT);
			
				var lines, sublines;
				var	u, v;
				
				lines = content[i].split('\r\n');
				for (u = 0; u < lines.length; u++) {
				
					if (u == lines.length - 1 && lines[u] == '')
					
						break;
				
					// Replace single '\r' characters by white spaces.
				
					lines[u] = lines[u].replace(/\r/g, ' ');
					
					// Cut lines made of single '\n' characters.
					
					sublines = lines[u].split('\n');
					for (v = 0; v < sublines.length; v++)

						if (v == sublines.length - 1 && sublines[v] == '')
					
							break;
					
						else {
						
							if (sublines[v].length > lineLimit)
								
								return false;
						
							formattedBody.push(sublines[v]);
							
						}

				}
				
			}
			
			body = formattedBody;
			return true;
			
		}
		
		// Return content of mail, its "parsed" body  (byte-stuffing removed) as an array of strings. Each string is a line.
		
		this.getContent = function () {
			
			var	content	= new Array();
			var	i;			
			
			for (i = 0; i < content.length; i++)
			
				if (body[i] == '..')
				
					content.push('.');
					
				else 
				
					content.push(body[i]);
			
			return content;
			
		} 
		
		// Send this mail using SMTP at address:port, using SSL if requested.
		// This function is blocking.
		
		this.send = function (address, port, isSSL, username, password) {
		
			if (typeof address != 'string' || typeof port != 'number')
			
				throw new MailException(MailException.INVALID_ARGUMENT);
		
			else {
			
				var smtp	= require(typeof requireNative != 'undefined' ? 'waf-mail/SMTP' : './SMTP.js');				

				return smtp.send(address, port, isSSL, username, password, this);
				
			}
			
		}
		
		// Parse an email as received from POP3 or IMAP. Take an array of lines, startLine and endLine are the start
		// and end (both included) indexes to read from the lines array. All arguments are mandatory. Return true if 
		// parsed successfully.
			
		this.parse = function (lines, startLine, endLine) {
			
			if (!(lines instanceof Array) || typeof startLine != 'number' || typeof endLine != 'number'
			|| startLine < 0 || startLine >= lines.length || endLine >= lines.length || endLine < startLine
			|| typeof lines[startLine] != 'string')

				throw new MailException(MailException.INVALID_ARGUMENT);
				
			var i;
					
			i = startLine;
			for ( ; ; ) {
				
				var match;
				
				if (lines[i] == '') {
					
					// Empty line separates header from body.
					
					break;	
						
				} else if ((match = lines[i].match(fieldNameDefinitionRegExp)) == null) {
				
					// Syntax error, cannot retrieve field name.
					
					return false;
					
				} else {
					
					var name, value;
						
					name = match[0].substr(0, match[0].length - 1);
					value = lines[i].length == match[0].length ? '' : lines[i].substr(match[0].length);
									
					// Unfold lines if needed.
					
					for ( ; ; )
					
						if (++i > endLine) {
						
							// An empty line must separate header from body (which can be empty).
					
							return false;
						
						} else if (typeof lines[i] != 'string')
				
							throw new MailException(MailException.INVALID_ARGUMENT);
					
						else if (lines[i].match(foldedLineStartRegExp) != null)

							value = value + '\r\n' + lines[i];
						
						else
							
							break;

					this.addField(name, value);

				}				
				
			}		

			// Step over empty line separator.
				
			i++;
			
			// Given body is supposed to be correctly formated (no line too long, byte-stuffed, no single '\r' or '\n'
			// characters).
				
			body = new Array();
			for ( ; i <= endLine; i++) 
				
				body.push(lines[i]);

			return true;
			
		}
		
		// Handle constructor call with arguments.
		
		if (arguments.length > 0) {
		
			if (arguments.length == 4) {
				
				if (typeof from != 'string' || typeof to != 'string' || typeof subject != 'string')
					
					throw new MailException(MailException.INVALID_ARGUMENT);					
				
				this.addField('From', from);
				this.addField('To', to);
				this.addField('Subject', subject);

				this.setContent(content);							
				
			} else if (arguments == 1 && arguments[0] instanceof Array) {

				// If mail has been received from POP3 or IMAP, it must be correct.
				
				if (!this.parse(arguments[0]))
				
					throw new MailException(MailException.INVALID_ARGUMENT);				
				
			} else if (arguments == 1 && typeof arguments[0] == 'object') {
				
				// JSON object describing a mail.
				// If a field is defined, it must be correctly. 
				// Body is set using setContent().
				
				var json	= arguments[0];
								
				if (typeof json.From != 'undefined')
					
					this.addField('From', json.From);				
				
				if (typeof json.To != 'undefined')
					
					this.addField('To', json.To);				
				
				if (typeof json.Subject != 'undefined')
					
					this.addField('Subject', json.Subject);
				
				if (typeof json.Content != 'undefined')
				
					this.setContent(json.Content);
	
			} else 
			
				throw new MailException(MailException.INVALID_ARGUMENT);
			
		}
		
	}
	
	return Mail;	
			
}

var Mail	= MailScope();

// Helper function to make a ready to send email.

var createMessage = function (from, recipient, subject, content) {
		
	var message	= new Mail();
			
	message.From = from;	
	if (recipient instanceof Array) {
	
		var	i;
		
		for (i = 0; i < recipient.length; i++)
		
			message.addField('To', recipient[i]);
	
	} else
	
		message.To = recipient;
		
	message.Subject = subject;
	message.setContent(content);
						
	return message;			
				
}

// Quick send function, see send() function of SMTP module.

var send = function (address, port, isSSL, username, password, from, recipient, subject, content) {

	var smtp	= require(typeof requireNative != 'undefined' ? 'waf-mail/SMTP' : './SMTP.js');				

	return smtp.send(address, port, isSSL, username, password, from, recipient, subject, content);
}

exports.createMessage = createMessage;
exports.send = send;
exports.Mail = Mail;
