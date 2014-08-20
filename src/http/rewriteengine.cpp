/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "rewriteengine.h"
#include <http/handlertype.h>
#include <http/handlerfactory.h>
#include <http/httpconnection.h>
#include <http/httpglobals.h>
#include <http/httplog.h>
#include <http/httpserverversion.h>
#include <http/httpvhost.h>
#include <http/requestvars.h>
#include <http/rewritemap.h>
#include <http/rewriterule.h>
#include <http/rewriterulelist.h>

#include <extensions/extworker.h>
#include <extensions/registry/extappregistry.h>

#include <util/accessdef.h>
#include <util/ni_fio.h>
#include <util/stringtool.h>

#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "httputil.h"



RewriteEngine::RewriteEngine()
{
}
RewriteEngine::~RewriteEngine()
{
}


int RewriteEngine::parseRules( char * &pRules, RewriteRuleList * pRuleList,
                        const RewriteMapList * pMapList )
{
    LinkedObj * pLast = pRuleList->tail();
    if ( !pLast )
        pLast = pRuleList->head();
    while( *pRules )
    {
        while( isspace( *pRules ) )
            ++pRules;
        if ( !*pRules )
            break;
        if ((( strncasecmp( pRules, "RewriteCond", 11 ) == 0 )&&
             ( isspace( *(pRules + 11)) ))||
            (( strncasecmp( pRules, "RewriteRule", 11 ) == 0 )&&
             ( isspace( *(pRules + 11)) )))
        {
            RewriteRule * pRule = new RewriteRule();
            if ( !pRule )
            {
                ERR_NO_MEM( "new RewriteRule()" );
                return -1;
            }
            int ret = pRule->parse( pRules, pMapList );
            if ( ret )
            {
                delete pRule;
                return 0;
            }
            pLast->addNext( pRule );
            pLast = pRule;
        }
        else
        {
            char * pLineEnd = strchr( pRules, '\n' );
            if ( pLineEnd )
                *pLineEnd = 0;
            if ( *pRules != '#' )
            {
                LOG_ERR(( "Invalid rewrite directive: %s", pRules ));
            }
            if ( pLineEnd )
                *pLineEnd = '\n';
            else
                break;
            pRules = pLineEnd + 1;
        }
    }
    return 0;
}

int RewriteEngine::appendUnparsedRule( AutoStr2 & sDirective,
                char * pBegin, char * pEnd )
{
    if ( m_qsLen + sDirective.len() + (pEnd - pBegin ) + 2 > REWRITE_BUF_SIZE * 4 )
        return -1;
    char * pBuf = m_rewriteBuf[0];
    memmove( &pBuf[m_qsLen], sDirective.c_str(), sDirective.len() );
    m_qsLen += sDirective.len();
    pBuf[m_qsLen++] = ' ';
    memmove( &pBuf[m_qsLen], pBegin, pEnd - pBegin );
    m_qsLen += pEnd - pBegin;
    pBuf[m_qsLen++] = '\n';
    pBuf[m_qsLen] = 0;
    return 0;
}

int RewriteEngine::parseUnparsedRules( RewriteRuleList *pRuleList,
                const RewriteMapList * pMapList )
{
    char * pRules = m_rewriteBuf[0];
    int ret = parseRules( pRules, pRuleList, pMapList );
    if ( pRules - m_rewriteBuf[0] >= m_qsLen )
        m_qsLen = 0;
    else
    {
        memmove( m_rewriteBuf[0], pRules, m_qsLen - ( pRules - m_rewriteBuf[0] ) );
        m_qsLen -= pRules - m_rewriteBuf[0];
    }
    return ret;
    
}

static const char s_hex[17] = "0123456789ABCDEF";


static char * escape_uri( char * p, const char * pURI, int uriLen )
{
    const char * pURIEnd = pURI + uriLen;
    register char ch;
    while( pURI < pURIEnd )
    {
        ch = *pURI++;
        if ( isalnum(ch) || (ch == '_') )
            *p++ = ch;
        else if ( ch == ' ' )
            *p++ = '+';
        else
        {
            *p++ = '%';
            *p++ = s_hex[ ((unsigned char )ch) >> 4 ];
            *p++ = s_hex[ ch & 0xf ];
        }
    }
    return p;
}

static int getSubstr( const char * pSource, const int *ovector, int matches, int i,
                          char * &pValue, int escape )
{
    if ( i < matches )
    {
        const int * pParam = ovector + ( i << 1 );
        if ( !escape )
        {
            pValue = (char *)pSource + *pParam;
            return *(pParam + 1) - *pParam;
        }
        else
        {
            char * pEnd = pValue;
            pEnd = escape_uri( pEnd, (char *)pSource + *pParam, *(pParam + 1) - *pParam );
            return pEnd - pValue;
        }
    }
    return 0;
}

int RewriteEngine::getSubstValue( const RewriteSubstItem * pItem, HttpConnection *pConn,
                        char * &pValue, int bufLen )
{
    HttpReq * pReq = pConn->getReq();
    int type = pItem->getType();
    int i;
    if ( type < REF_STRING )
    {
        pValue = (char *)pReq->getHeader( type );
        if ( *pValue )
            return pReq->getHeaderLen( type );
        else
            return 0;
    }

/*
    if ( type >= REF_TIME )
    {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        switch( type )
        {
        case REF_TIME:
            i = snprintf( pValue, bufLen,
                        "%04d%02d%02d%02d%02d%02d", tm->tm_year + 1900,
                        tm->tm_mon+1, tm->tm_mday,
                        tm->tm_hour, tm->tm_min, tm->tm_sec);
            break;
        case REF_TIME_YEAR:
            i = snprintf( pValue, bufLen, "%04d", tm->tm_year + 1900);
            break;
        case REF_TIME_MON:
            i = snprintf( pValue, bufLen, "%02d", tm->tm_mon+1 );
            break;
        case REF_TIME_DAY:
            i = snprintf( pValue, bufLen, "%02d", tm->tm_mday);
            break;
        case REF_TIME_HOUR:
            i = snprintf( pValue, bufLen, "%02d", tm->tm_hour);
            break;
        case REF_TIME_MIN:
            i = snprintf( pValue, bufLen, "%02d", tm->tm_min);
            break;
        case REF_TIME_SEC:
            i = snprintf( pValue, bufLen, "%02d", tm->tm_sec);
            break;
        case REF_TIME_WDAY:
            i = snprintf( pValue, bufLen, "%d", tm->tm_wday);
            break;
        default:
            return 0;
        }
        return i;
    }
*/
    switch( type )
    {
    case REF_STRING:
        pValue = (char *)pItem->getStr()->c_str();
        return pItem->getStr()->len();
    case REF_MAP:
        {
            MapRefItem * pRef = pItem->getMapRef();
            int len = 1024;
            char achBuf[1024];
            if ( buildString( pRef->getKeyFormat(), pConn, achBuf, len ) == NULL )
                return 0;
            if ( (len = pRef->getMap()->lookup( achBuf, len, pValue, bufLen )) == -1 )
            {
                if ( pRef->getDefaultFormat() )
                {
                    if ( buildString( pRef->getDefaultFormat(), pConn,
                                    pValue, bufLen ) == NULL )
                        return 0;
                    len = bufLen;
                }        
                else
                    len = 0;
            }
            return len;
        }
        break;             
    case REF_RULE_SUBSTR:
        return getSubstr( m_pSourceURL, m_ruleVec, m_ruleMatches, pItem->getIndex(),
                    pValue, m_flag & RULE_FLAG_BR_ESCAPE );
    case REF_COND_SUBSTR:
        return getSubstr( m_pCondBuf, m_condVec, m_condMatches, pItem->getIndex(),
                    pValue, m_flag & RULE_FLAG_BR_ESCAPE );
    case REF_ENV:
        pValue = (char *)RequestVars::getEnv( pConn, pItem->getStr()->c_str(), 
                    pItem->getStr()->len(), i );
        if ( !pValue )
        {
            i = 0;
        }
        return i;
    case REF_HTTP_HEADER:
        pValue = (char *)pReq->getHeader( pItem->getStr()->c_str(),
                        pItem->getStr()->len(), i );
        if ( !pValue )
            i = 0;
        return i;
    case REF_REQUST_FN:
    case REF_SCRIPTFILENAME:
        if ( m_iScriptLen == -1 )
        {
            pReq->checkPathInfo( m_pSourceURL, m_sourceURLLen, m_iFilePathLen,
                            m_iScriptLen, m_iPathInfoLen, m_pContext );
        }
        pValue = HttpGlobals::g_achBuf;
        //if ( m_pStrip )
        //    return m_iFilePathLen;
        //else

        //   return m_iFilePathLen + m_iPathInfoLen;
        return m_iFilePathLen;
    case REF_PATH_INFO:
        if ( m_iScriptLen == -1 )
        {
            pReq->checkPathInfo( m_pSourceURL, m_sourceURLLen, m_iFilePathLen,
                            m_iScriptLen, m_iPathInfoLen, m_pContext );
        }
        pValue = &HttpGlobals::g_achBuf[ m_iFilePathLen ];
        return m_iPathInfoLen;
    case REF_SCRIPT_NAME:
        if ( m_iScriptLen == -1 )
        {
            pReq->checkPathInfo( m_pSourceURL, m_sourceURLLen, m_iFilePathLen,
                            m_iScriptLen, m_iPathInfoLen, m_pContext );
        }
        pValue = (char *)pReq->getOrgReqURL();
        return m_iScriptLen;
    case REF_REQ_URI:   //in rewrite rule, this does not include Query String part
        if ( pReq->getRedirects() == 0 )
        {
            pValue = (char *)pReq->getOrgReqURL();
            return pReq->getOrgReqURILen();
        }
        if ( m_pStrip )
        {
            memmove( pValue, m_pStrip->c_str(), m_pStrip->len() );
            memmove( pValue + m_pStrip->len(), m_pSourceURL, m_sourceURLLen );
            return m_pStrip->len() + m_sourceURLLen;
        }
        //fall through
    case REF_CUR_REWRITE_URI:
        pValue = (char *)m_pSourceURL;
        return m_sourceURLLen;
    case REF_QUERY_STRING:
        pValue = (char *)m_pQS;
        return m_qsLen;
    default:
        return RequestVars::getReqVar( pConn, type, pValue, bufLen );
    }
    return 0;
}

extern int transform_urlDecode( const char * org, int org_len, char * dest, int dest_len, int * changed);

int RewriteEngine::appendSubst( const RewriteSubstItem * pItem, HttpConnection *pConn,
                        char * &pBegin, char *pBufEnd, int &esc_uri, int noDupSlash )
{
    char * pValue = pBegin;
    int valLen = getSubstValue( pItem, pConn, pValue, pBufEnd - pValue );
    if ( valLen <= 0 )
        return valLen;
    if ( pBufEnd <= pBegin + valLen )
        return 0;
    if ( pValue != pBegin )
    {
        if (( *pValue == '/' )&&( noDupSlash )&&(pBegin[-1] == '/'))
            --pBegin;
        memmove( pBegin, pValue, valLen );
    }
    else
    {
        if (( *pValue == '/' )&&( noDupSlash )&&(pBegin[-1] == '/'))
        {
            --valLen;
            memmove( pBegin, pBegin+1, valLen );
        }
    }
    if ( esc_uri && (valLen > 0 ))
    {
        int len = valLen;
        char *pEnd;
        pEnd = (char *)memchr( pBegin, '?', valLen );
        if ( pEnd )
        {
            esc_uri = 0;
            len = pEnd - pBegin;
        }
        if (( len > 0 )&&( pItem->needUrlDecode() ))
        {
            int c;
            //int l1 = transform_urlDecode( pBegin, len, pBegin, len, &c );
            int l1 = HttpUtil::unescape_n( pBegin, len, pBegin, len );
            c = len - l1;
            if ( c > 0 )
            {
                if ( len < valLen )
                    memmove( pBegin + l1, pBegin + len, valLen - len );
                valLen -= c;
            }
        }
    }
    pBegin += valLen;
    return 0;
}

char * RewriteEngine::buildString( const RewriteSubstFormat * pFormat, HttpConnection * pConn,
                          char * pBuf, int &len, int esc_uri, int noDupSlash )
{
    char * pBegin = pBuf;
    char * pBufEnd = pBuf + len - 1;
    const RewriteSubstItem * pItem = pFormat->begin();
    
//    if ( !pItem->next() )
//    {
//        int valLen = getSubstValue( pItem, pConn, pBegin, pBufEnd );
//        return valLen;
//        //only one variable, no need to copy to buffer
//    }

    while( pItem )
    {
        if ( appendSubst( pItem, pConn, pBegin, pBufEnd, esc_uri,
                        (pBegin>pBuf)?noDupSlash:0 ) == -1 )
            return NULL;
        pItem = (const RewriteSubstItem *)pItem->next();
    }
    *pBegin = 0;
    len = pBegin - pBuf;
    return pBuf;
}

int RewriteEngine::processCond( const RewriteCond * pCond, HttpConnection *pConn )
{
    int len = REWRITE_BUF_SIZE - 1;
    char * pTest;
    if ( m_pLastCondStr && m_pLastCondStr->equal( * pCond->getTestStringFormat() ) )
    {
        pTest = m_pLastTestStr;
        len = m_lastTestStrLen;
    }
    else
    {
        pTest = buildString( pCond->getTestStringFormat(),
                                      pConn, m_pFreeBuf, len, 1, 1 );
        m_pLastCondStr = pCond->getTestStringFormat();
        m_pLastTestStr = pTest;
        m_lastTestStrLen = len;
        m_noStat = 1;
    }
    int ret = 0;
    if ( !pTest )
        return -1;
    int condVec[ MAX_REWRITE_MATCH * 3 ];
    int condMatches = 0;
    //m_condMatches = 0;
    int code = pCond->getOpcode();
    if ( code == COND_OP_REGEX )
    {
        ret = pCond->getRegex()->exec( pTest, len, 0, 0, condVec,
                                       MAX_REWRITE_MATCH * 3);
        if ( m_logLevel > 2 )
            LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] Cond: Match '%s' with pattern '%s', result: %d",
                pConn->getLogId(), pTest, pCond->getPattern(), ret ));
        if ( ret == 0 )
            condMatches = 10;
        else
            condMatches = ret;
        ret = (ret < 0 );
    }
    else if (( code >= COND_OP_LESS)&&( code <= COND_OP_EQ ))
    {
        if ( pCond->getFlag() & COND_FLAG_NOCASE )
            ret = strcasecmp( pTest, pCond->getPattern() );
        else
            ret = strcmp( pTest, pCond->getPattern() );
        if ( m_logLevel > 2 )
            LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] Cond: Compare '%s' with pattern '%s', result: %d",
                pConn->getLogId(), pTest, pCond->getPattern(), ret ));
        switch( code )
        {
        case COND_OP_LESS:
            ret = ( ret >= 0 );
            break;
        case COND_OP_GREATER:
            ret = ( ret <= 0 );
            break;
        case COND_OP_EQ:
            ret = ( ret != 0 );
            break;
        }        
    }
    else if (( code >= COND_OP_DIR )&& (code <= COND_OP_FILE_ACC ))
    {
        if ( m_noStat == 1 )
            m_noStat = nio_stat( pTest, &m_st );
        if ( m_noStat == 0 )
        {
            switch( code )
            {
            case COND_OP_DIR:
                ret = !S_ISDIR( m_st.st_mode );
                break;
            case COND_OP_FILE:
                ret = !S_ISREG( m_st.st_mode );
                break;
            case COND_OP_SIZE:
                ret = (!S_ISREG( m_st.st_mode )||( m_st.st_size == 0 ));
                break;
            case COND_OP_SYM:
                ret = !S_ISLNK( m_st.st_mode );
                break;
            case COND_OP_FILE_ACC:
                ret = !S_ISREG( m_st.st_mode );
                break;
            default:
                ret = -1;
            }
            if ( m_logLevel > 2 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] Cond: test '%s' with pattern '%s', result: %d",
                    pConn->getLogId(), pTest, pCond->getPattern(), ret ));
        }
        else
        {
            if ( m_logLevel > 2 )
                LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] stat( %s ) failed ",
                pConn->getLogId(), pTest ));
            ret = -1;
        }
        
    }
    else if ( code == COND_OP_URL_ACC )
    {
        //do nothing.
    }
    else
        assert( 0 );
    if (!(pCond->getFlag() & COND_FLAG_NOMATCH) )
    {
        if ( condMatches > 0 )
        {
            if ( pTest == m_pFreeBuf )
            {
                m_pFreeBuf = m_pCondBuf;
                m_pCondBuf = pTest;
            }
            m_condMatches = condMatches;
            memmove( m_condVec, condVec, condMatches * 3 * sizeof( int ) );
        }
        return ret;
    }
    else
        return !ret;
    
}

int RewriteEngine::processRule( const RewriteRule * pRule, HttpConnection *pConn )
{
    m_ruleMatches = 0;
    int ret = pRule->getRegex()->exec( m_pSourceURL, m_sourceURLLen, 0,
                                       0, m_ruleVec, MAX_REWRITE_MATCH * 3 );
    if ( m_logLevel > 1 )
        LOG_INFO(( pConn->getLogger(),
            "[%s] [REWRITE] Rule: Match '%s' with pattern '%s', result: %d",
            pConn->getLogId(), m_pSourceURL, pRule->getPattern(), ret ));
    if ( ret < 0 )
    {
        if (!(pRule->getFlag() & RULE_FLAG_NOMATCH) )
            return -1;
    }
    else
    {
        if ((pRule->getFlag() & RULE_FLAG_NOMATCH) )
            return -1;
        if ( ret == 0 )
            m_ruleMatches = 10;
        else
            m_ruleMatches = ret;
    }
    m_pLastCondStr = NULL;
    m_condMatches = 0;
    const RewriteCond * pCond = pRule->getFirstCond();
    while( pCond )
    {
        if ( processCond( pCond, pConn ) )
        {
            if (!(( pCond->getFlag() & COND_FLAG_OR )&&
                ( pCond->next() )) )
                return -1;
        }
        else
        {
            while(( pCond->getFlag() & COND_FLAG_OR )&&( pCond->next() ))
                pCond = (RewriteCond *)pCond->next();
        }
        pCond = (RewriteCond *)pCond->next();
    }
    return processRewrite( pRule, pConn );
}

static int isAbsoluteURI( const char * pURI, int len )
{
    if (( *pURI == '/' )||( len < 6 ))
        return 0;

    switch( *pURI++ )
    {
    case 'f': case 'F':
        if ( strncasecmp( pURI, "tp://", 5 ) == 0 )
            return 6;
        break;
    case 'g': case 'G':
        if ( strncasecmp( pURI, "opher://", 8 ) == 0 )
            return 9;
        break;
    case 'h': case 'H':
        if ( strncasecmp( pURI, "ttp://", 6 ) == 0 )
            return 7;
        if ( strncasecmp( pURI, "ttps://", 7 ) == 0 )
            return 8;
        break;
    case 'm': case 'M':
        if ( strncasecmp( pURI, "ailto:", 6 ) == 0 )
            return 7;
        break;
    case 'n': case 'N':
        if ( strncasecmp( pURI, "ews:", 4 ) == 0 )
            return 5;
        if ( strncasecmp( pURI, "ntp://", 6 ) == 0 )
            return 7;
        break;
    }
    return 0;
}


int RewriteEngine::processQueryString( HttpConnection * pConn, int flag )
{
    char * pBuf = (char *)strchr( m_pSourceURL, '?' );
    if ( flag & RULE_FLAG_QSDISCARD )
    {
        m_pQS = m_qsBuf;
        m_qsLen = 0;
        m_qsBuf[m_qsLen] = 0;
        pConn->getReq()->orContextState( REWRITE_QSD );
    }
    if ( !pBuf )
        return 0;
    *pBuf = 0;
    int n = m_sourceURLLen - 1 - (pBuf - m_pSourceURL);
    m_sourceURLLen = pBuf - m_pSourceURL;
    pBuf++;
    if ( n > 0 )
    {
        if ( n > REWRITE_BUF_SIZE - 1 )
            n = REWRITE_BUF_SIZE - 1;
        if ( flag & RULE_FLAG_QSAPPEND )
        {
            if( REWRITE_BUF_SIZE < n + m_qsLen + 2 )
            {
                m_qsLen = REWRITE_BUF_SIZE - n - 2;
                if ( m_qsLen < 0 )
                    m_qsLen = 0;
            }
            if ( m_qsLen > 0 )
            {
                memmove( &m_qsBuf[n+1], m_pQS, m_qsLen );
                m_qsBuf[n] = '&';
                ++m_qsLen;
            }
            if ( m_logLevel > 1 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] append query string '%s'",
                    pConn->getLogId(), pBuf ));
        }
        else
        {
            if ( m_logLevel > 1 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] replace current query string with '%s'",
                    pConn->getLogId(), pBuf ));
            m_qsLen = 0;
        }
        memmove( m_qsBuf, pBuf, n );
        m_qsLen = n + m_qsLen;
        m_pQS = m_qsBuf;
        if ( m_qsBuf[m_qsLen - 1] == '&' )
            --m_qsLen;
        m_qsBuf[m_qsLen] = 0;
    }
    else
    {
        if (!( flag & RULE_FLAG_QSAPPEND ))
        {
            if ( m_logLevel > 1 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] remove current query string",
                    pConn->getLogId() ));
            m_pQS = m_qsBuf;
            m_qsLen = 0;
            m_qsBuf[m_qsLen] = 0;
        }
    }
    return 0;
}

int RewriteEngine::setCookie( char * pBuf, int len, HttpConnection * pConn )
{
    char * pName;
    char * pVal;
    char * pDomain;
    char * pPath;
    char * pSave;
    int secure = 0;
    int httponly = 0;
    int age = 0;
    pName = strtok_r( pBuf, ":", &pSave );
    pVal = strtok_r( NULL, ":", &pSave );
    pDomain = strtok_r( NULL, ":", &pSave );
    char *p = strtok_r( NULL, ":", &pSave );
    age = atoi( p );
    pPath = strtok_r( NULL, ":", &pSave );
    if ( pPath )
    {
        if ( *pPath != '/' )
            pPath = NULL;
    }
    p = strtok_r( NULL, ":", &pSave );
    if ( p )
    {
        secure = (( *p == '1' )||( strncasecmp( "true", p, 4 ) == 0 )
                   ||( strncasecmp( "secure", p, 6 ) == 0 ));
    }
    p = strtok_r( NULL, ":", &pSave );
    if ( p )
    {
        httponly = (( *p == '1' )||( strncasecmp( "true", p, 4 ) == 0 )
                   ||( strncasecmp( "HttpOnly", p, 8 ) == 0 )); 
    }
    return pConn->getResp()->addCookie( pName, pVal, pPath, pDomain, age, secure, httponly );
    
}

int RewriteEngine::expandEnv( const RewriteRule * pRule, HttpConnection * pConn )
{
    RewriteSubstFormat * pEnv = pRule->getEnv()->begin();
    const char * pKey;
    const char * pKeyEnd;
    const char * pValue;
    const char * pValEnd;
    int len = REWRITE_BUF_SIZE;
    char achBuf[REWRITE_BUF_SIZE];
    if ( !pEnv )
        return 0;
    while( pEnv )
    {
        len = REWRITE_BUF_SIZE - 1;
        buildString( pEnv, pConn, achBuf, len );
        if ( pEnv->isCookie() )
        {
            //FIXME: enable it later
            setCookie( achBuf, len, pConn );
            pEnv = (RewriteSubstFormat *)pEnv->next();
            continue;
        }
        pKeyEnd = strchr( achBuf, ':' );
        if ( pKeyEnd )
        {
            pKey = achBuf;
            pValue = pKeyEnd + 1;
            pValEnd = &achBuf[len];
            StringTool::strtrim( pKey, pKeyEnd );
            StringTool::strtrim( pValue, pValEnd );
            *(char*)pKeyEnd = 0;
            *(char*)pValEnd = 0;
            if ( strcasecmp( pKey, "dontlog" ) == 0 )
            {
                if ( m_logLevel > 4 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] disable access log for this request.",
                         pConn->getLogId() ));
                pConn->getResp()->needLogAccess(0);
            }
            else if ( strcasecmp( pKey, "nokeepalive" ) == 0 )
            {
                if ( m_logLevel > 4 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] turn off connection keepalive.",
                         pConn->getLogId() ));
                pConn->getReq()->keepAlive( false );
            }
//            else if ( strcasecmp( pKey, "noconntimeout" ) == 0 )
//            {
//                if ( m_logLevel > 4 )
//                    LOG_INFO(( pConn->getLogger(),
//                        "[%s] [REWRITE] turn off connection timeout.",
//                         pConn->getLogId() ));
//                pConn->getReq()->orContextState( NO_CONN_TIMEOUT );
//            }
            else if (( pRule->getAction() == RULE_ACTION_PROXY)&&
                    ( strcasecmp( pKey, "Proxy-Host" ) == 0 ))
            {
                pConn->getReq()->setNewHost( pValue, 
                        pValEnd - pValue );
                if ( m_logLevel > 4 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] Set Proxy Host header to: '%s' ",
                    pConn->getLogId(), pKey, pValue ));
            }
            else if ( strcasecmp( pKey, "no-gzip" ) == 0 )
            {
                if ( strncmp( pValue, "0", 1 ) != 0 )
                {
                    if ( m_logLevel > 4 )
                        LOG_INFO(( pConn->getLogger(),
                          "[%s] [REWRITE] turn off gzip compression for this requst.",
                             pConn->getLogId()));
                    pConn->getReq()->andGzip( ~GZIP_ENABLED );
                }
            }
            else
            {
                pConn->getReq()->addEnv( pKey, pKeyEnd - pKey, pValue, pValEnd - pValue );
                if ( m_logLevel > 4 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] add ENV: '%s:%s' ",
                        pConn->getLogId(), pKey, pValue ));
            }
        }
        pEnv = (RewriteSubstFormat *)pEnv->next();
    }
    return 0;
}

int RewriteEngine::processRewrite( const RewriteRule * pRule, HttpConnection *pConn )
{
    char * pBuf;
    int flag = pRule->getFlag();
    expandEnv( pRule, pConn );
    m_rewritten |= 1;
    if (!( flag & RULE_FLAG_NOREWRITE ))
    {
        int len = REWRITE_BUF_SIZE - 1;
        pBuf = m_pDestURL;
        m_pDestURL = m_pFreeBuf;
        m_pFreeBuf = pBuf;
        m_flag = flag;
        pBuf = buildString( pRule->getTargetFmt(), pConn, m_pDestURL, len, 1, 1 );
        // log rewrite result here
        if ( !pBuf )
        {
            if ( m_logLevel > 0 )
                LOG_ERR(( pConn->getLogger(),
                    "[%s] [REWRITE] Failed to build the target URI",
                        pConn->getLogId() ));
            return -1;
        }

//         if ( strchr( pBuf, '%' ) )
//         {
//             int c;
//             int l, l1;
//             char * pEnd = strchr( pBuf, '?' );
//             if ( pEnd )
//                 l = pEnd - pBuf;
//             else
//                 l = len;
//             l1 = transform_urlDecode( pBuf, l, pBuf, l, &c );
//             len -= l - l1;
//         }

        if ( m_logLevel > 0 )
            LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] Source URI: '%s' => Result URI: '%s'",
                    pConn->getLogId(), m_pSourceURL, pBuf ));
        m_rewritten |= 2;
        m_pOrgSourceURL = m_pSourceURL;
        m_orgSourceURLLen = m_sourceURLLen;
        m_pSourceURL = pBuf;
        m_sourceURLLen = len;
        m_iScriptLen = -1;
        m_iPathInfoLen = 0;
        if ( flag & (RULE_FLAG_WITHQS|RULE_FLAG_QSDISCARD) )
        {
            processQueryString( pConn, flag );
        }
    }
    else if ( m_logLevel > 0 )
        LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] No substition",
                pConn->getLogId()));
    
    if ( pRule->getAction() != RULE_ACTION_NONE )
    {
        m_action = pRule->getAction();
        m_statusCode = pRule->getStatusCode();
        return 0;
    }

    if ( pRule->getMimeType() )
    {
        if ( m_logLevel > 4 )
            LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] set forced type: '%s'",
                pConn->getLogId(), pRule->getMimeType() ));
        pConn->getReq()->setForcedType( pRule->getMimeType() );
    }
    return 0;
}

const RewriteRule * RewriteEngine::getNextRule( const RewriteRule * pRule, 
                    const HttpContext * &pContext, const HttpContext * &pRootContext )
{
    const RewriteRule * pNext = NULL;
    if ( pRule )
        pNext = (const RewriteRule *) pRule->next();
    if ( pNext )
        return pNext;
    if ( !pContext && pRootContext )    //special case for inherit global rewrite rules
    {
        pContext = pRootContext;
        pRootContext = pContext->getParent();
        const RewriteRuleList * pList = pContext->getRewriteRules();
        if ( pList )
        {
            return pList->begin();
        }
        return NULL;
    }
    while(( pContext && (pContext->getConfigBits() & BIT_REWRITE_INHERIT) )&&
          ( pContext->getParent() != pRootContext ))   
    {
        pContext = pContext->getParent();
        if ( !pContext->hasRewriteConfig() )
            continue;
        const RewriteRuleList * pList = pContext->getRewriteRules();
        if ( pList )
        {
            pNext = pList->begin();
            break;
        }
    }
    return pNext;
}

int RewriteEngine::processRuleSet( const RewriteRuleList * pRuleList, HttpConnection * pConn,
            const HttpContext * pContext, const HttpContext * pRootContext )
{
    const RewriteRule * pRule = NULL;
    int loopCount = 0;
    int flag = 0;
    int ret;
    m_pContext = pContext;
    if ( pRuleList )
        pRule = pRuleList->begin();
    else
        pRule = getNextRule( NULL, pContext, pRootContext );
    if ( !pRule )
        return 0;
    HttpReq * pReq = pConn->getReq();
    const AutoStr2 * pBase = NULL;
    AutoStr2    sStrip;
    m_rewritten = 0;
    //initialize rewrite engine
    //strip prefix aka. RewriteBase
    m_logLevel = pReq->getRewriteLogLevel();
    m_pSourceURL = pReq->getURI();
    m_sourceURLLen = pReq->getURILen();
    m_pStrip = m_pBase = NULL;
    m_iScriptLen = -1;
    m_iPathInfoLen = 0;
    if ( m_pContext )
    {
        pBase = m_pContext->getContextURI();
        if (( pBase )&&
            ( strncmp( m_pSourceURL, pBase->c_str(), pBase->len() ) == 0 ))
        {
            m_pStrip = m_pBase = pBase;
        }
        else
        {
            m_pBase = pBase = m_pContext->getRewriteBase();
            if (( pBase )&&
                ( strncmp( m_pSourceURL, pBase->c_str(), pBase->len() ) == 0 ))
            {
                m_pStrip = m_pBase = pBase;
            }
            
        }
        
        if ( m_pContext->getRewriteBase() )
            m_pBase = m_pContext->getRewriteBase();
        if ( m_pStrip )
        {
            if ( m_logLevel > 4 )
                LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] strip base: '%s' from URI: '%s'",
                        pConn->getLogId(), m_pStrip->c_str(), m_pSourceURL ));
            m_pSourceURL += m_pStrip->len();
            m_sourceURLLen -= m_pStrip->len();
        }
        else
        {
            if ( pConn->getReq()->isMatched() )
            {
                const char * pURL;
                int len;
                pConn->getReq()->stripRewriteBase( m_pContext,
                    pURL, len );
                if (( len < m_sourceURLLen )&&( strncmp( 
                        m_pSourceURL + m_sourceURLLen - len, pURL, len ) == 0 ))
                {
                    sStrip.setStr( m_pSourceURL, m_sourceURLLen - len );
                    m_pStrip = &sStrip;
                    if ( !m_pBase )
                        m_pBase = m_pStrip;
                }
                m_pSourceURL = pURL;
                m_sourceURLLen = len;
            }
        }
    }

    m_pQS = pReq->getQueryString();
    m_qsLen = pReq->getQueryStringLen();

    m_pOrgSourceURL = m_pSourceURL;
    m_orgSourceURLLen = m_sourceURLLen;

    m_condMatches = 0;
    m_pDestURLLen = 0;
    m_pDestURL = m_rewriteBuf[0];
    m_pCondBuf = m_rewriteBuf[1];
    m_pFreeBuf = m_rewriteBuf[2];
    m_action   = RULE_ACTION_NONE;
    m_flag     = 0;
    m_statusCode = 0;
    while( pRule )
    {
        flag = pRule->getFlag();
//        if (( flag & RULE_FLAG_NOSUBREQ )&&( pReq->isSubReq() > 0 ))
//            ret = -1;
//        else
            ret = processRule( pRule, pConn );
        if ( ret )
        {
            pRule = getNextRule( pRule, pContext, pRootContext );
            while( pRule && ( flag & RULE_FLAG_CHAIN ))
            {
                if ( m_logLevel > 5 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] skip chained rule: '%s'",
                        pConn->getLogId(), pRule->getPattern() ));
                flag = pRule->getFlag();
                pRule = getNextRule( pRule, pContext, pRootContext );
                            //(const RewriteRule *) pRule->next();
            }
            continue;
        }
        if (( flag & RULE_FLAG_LAST )&&!pRule->getSkip())
        {
            if ( m_logLevel > 5 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] Last Rule, stop!",
                    pConn->getLogId() ));
            if ( flag & RULE_FLAG_END )
            {
                if ( m_logLevel > 5 )
                    LOG_INFO(( pConn->getLogger(),
                        "[%s] [REWRITE] End rewrite!",
                        pConn->getLogId() ));
                pConn->getReq()->orContextState( SKIP_REWRITE );
            }
            break;
        }
NEXT_RULE:
        if ( flag & RULE_FLAG_NEXT )
        {
            pContext = m_pContext;
            if ( pRuleList )
                pRule = pRuleList->begin();
            else
                pRule = getNextRule( NULL, pContext, pRootContext );
            if ( ++loopCount > 10 )
            {
                LOG_ERR(( pConn->getLogger(),
                        "[%s] [REWRITE] Rules loop 10 times, possible infinite loop!",
                        pConn->getLogId() ));
                break;
            }
            if ( m_logLevel > 5 )
                LOG_INFO(( pConn->getLogger(),
                    "[%s] [REWRITE] Next round, restart from the first rule",
                    pConn->getLogId() ));
            continue;
        }
        if ( !pRule )
            break;
        int n = pRule->getSkip()+1;
        if (( n > 1 )&&( m_logLevel > 5 ))
            LOG_INFO(( pConn->getLogger(),
                "[%s] [REWRITE] skip next %d rules",
                pConn->getLogId(), n - 1 ));
        while( pRule && n > 0 )
        {
            pRule = getNextRule( pRule, pContext, pRootContext );
                    //(const RewriteRule *) pRule->next();
            --n;
        }
    }
    if ( m_rewritten )
    {
        if (( m_action == RULE_ACTION_FORBID )||
            ( m_action == RULE_ACTION_GONE   ))
            return m_statusCode;
        if ( m_rewritten & 2 )
        {
            //set the final URL and query string
            if ( ! isAbsoluteURI( m_pSourceURL, m_sourceURLLen ) )
            {
                if ( *m_pSourceURL != '/' )
                {
                    // add missing prefix (RewriteBase)
                    char * pBuf = m_pFreeBuf;
                    int baseLen;
                    if ( !m_pBase )
                    {
                        baseLen   = 1;
                        *pBuf = '/';
                    }
                    else
                    {
                        baseLen = m_pBase->len();
                        memmove( pBuf, m_pBase->c_str(), baseLen );
                    }
                    if ( m_sourceURLLen > REWRITE_BUF_SIZE - 1 - baseLen )
                        m_sourceURLLen = REWRITE_BUF_SIZE - 1 - baseLen;
                    memmove( pBuf + baseLen, m_pSourceURL, m_sourceURLLen );
                    m_pFreeBuf = (char *)m_pSourceURL;
                    m_pSourceURL = pBuf;
                    m_sourceURLLen += baseLen;
                    pBuf[m_sourceURLLen] = 0;
                    if (( m_logLevel > 4 )&&( m_pBase ))
                        LOG_INFO(( pConn->getLogger(),
                                "[%s] [REWRITE] prepend rewrite base: '%s', final URI: '%s'",
                                pConn->getLogId(), m_pBase->c_str(), m_pSourceURL ));
                }
            }
            else if ( m_action == RULE_ACTION_NONE )
            {
                m_action = RULE_ACTION_REDIRECT;
                m_statusCode = SC_302;
            }
            if ( m_action == RULE_ACTION_NONE )
            {
                if ( !pReq->getRedirects() || pBase)
                {
                    ret = pReq->saveCurURL();
                    if ( ret )
                        return ret;
                }
                if ( m_pQS == m_qsBuf )
                    pReq->setRewriteQueryString( m_pQS, m_qsLen );
                m_statusCode = -3;  //rewritten to another url
            }
            else if ( m_action == RULE_ACTION_REDIRECT )
            {
                if ( pReq->detectLoopRedirect( (char *)m_pSourceURL, m_sourceURLLen,
                                        m_pQS, m_qsLen, pConn->isSSL() ) == 0 )
                {
                    pReq->setRewriteLocation( (char *)m_pSourceURL, m_sourceURLLen,
                                        m_pQS, m_qsLen, m_flag & RULE_FLAG_NOESCAPE );
                    pReq->orContextState( REWRITE_REDIR );
                }
                else
                {
                    LOG_INFO(( pConn->getLogger(),
                            "[%s] [REWRITE] detect external loop redirection with target URL: %s, skip.",
                                pConn->getLogId(), m_pSourceURL ));
                    m_rewritten = m_statusCode = 0;
                    m_pSourceURL = m_pOrgSourceURL;
                    m_sourceURLLen = m_orgSourceURLLen ;

                    goto NEXT_RULE;
                }
            }
            else if ( m_action == RULE_ACTION_PROXY )
            {
                if ( strncasecmp( m_pSourceURL, "http://", 7 ) != 0 )
                {
                    LOG_ERR(( "[REWRITE] Absolute URL with leading 'http://' is "
                              "required for proxy, URL: %s", m_pSourceURL ));
                    return SC_500;
                }
                char * pHost = (char *)m_pSourceURL + 7;
                char * pHostEnd = strchr( pHost, '/' );
                if (( !pHostEnd )||(pHostEnd == pHost))
                {
                    LOG_ERR(( "[REWRITE] Can not determine proxy host name" ));
                    return SC_500;
                }
                *pHostEnd = 0;
                const HttpHandler * pHandler = HandlerFactory::getInstance(
                        HandlerType::HT_PROXY, pHost );
                if ( !pHandler )
                {
                    LOG_ERR(( "[REWRITE] Proxy target is not defined on "
                              "external application list, please add a 'web server'"
                              " with name '%s'", pHost ));
                    return SC_500;
                }
                *pHostEnd = '/';
                pReq->setHandler( pHandler );

                //FIXME: change request header
                ret = pReq->internalRedirectURI( pHostEnd, 
                        m_pSourceURL + m_sourceURLLen - pHostEnd, 0,
                        m_flag & RULE_FLAG_NOESCAPE );
                if ( ret )
                    return SC_500;

                if ( m_pQS == m_qsBuf )
                    pReq->setRewriteQueryString( m_pQS, m_qsLen );
            }
        }
        else if ( m_action == RULE_ACTION_REDIRECT )
        {
            return 0;
        }
        return m_statusCode;
    }
    return 0;
}




