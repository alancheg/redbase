//
//
#include "ix_indexscan.h"
#include <cerrno>
#include <cassert>
#include <cstdio>
#include <iostream>

using namespace std;

IX_IndexScan::IX_IndexScan(): bOpen(false), desc(false), eof(false), lastNode(NULL)
{
  pred = NULL;
  pixh = NULL;
  currNode = NULL;
  currPos = -1;
}

IX_IndexScan::~IX_IndexScan()
{
  // in case close was forgotten
  if (pred != NULL)
    delete pred;
  
  if(pixh != NULL && pixh->GetHeight() > 1) {
    if(currNode != NULL)
      delete currNode;
    if(lastNode != NULL)
      delete lastNode;
  }
}


RC IX_IndexScan::OpenScan(const IX_IndexHandle &fileHandle,
                          CompOp     compOp,
                          void       *value,
                          ClientHint pinHint,
                          bool desc) 
{
  if (bOpen)
  {
    // scan is already open
    return IX_HANDLEOPEN;
  }

  if((compOp < NO_OP) ||
      compOp > GE_OP)
    return IX_FCREATEFAIL;


  pixh = const_cast<IX_IndexHandle*>(&fileHandle);
  if((pixh == NULL) ||
     pixh->IsValid() != 0)
    return IX_FCREATEFAIL;

  bOpen = true;
  if(desc) 
    this->desc = true;

  pred = new Predicate(pixh->GetAttrType(),
                       pixh->GetAttrLength(),
                       0,
                       compOp,
                       value,
                       pinHint);

  if(value != NULL)
    OpOptimize(compOp, value);
  
  // pixh->Print(cerr);
  return 0;
}

RC IX_IndexScan::GetNextEntry     (RID &rid)
{
  void * k = NULL;
  int i = -1;
  return GetNextEntry(k, rid, i);
}

RC IX_IndexScan::GetNextEntry(void *& k, RID &rid, int& numScanned)
{
  if(!bOpen)
    return IX_FNOTOPEN;
  assert(pixh != NULL && pred != NULL && bOpen);
  if(eof)
    return IX_EOF;

  // first time in
  if(currNode == NULL && currPos == -1) {
    // pixh->Print(cerr);
    if(!desc) {
      currNode = pixh->FetchNode(pixh->FindSmallestLeaf()->GetPageRID());
      currPos = -1;
    } else {
      currNode = pixh->FetchNode(pixh->FindLargestLeaf()->GetPageRID());
      currPos = currNode->GetNumKeys(); // 1 past
    }
  }
  
  for( ;
       (currNode != NULL);
       /* see end of loop */ ) 
  {
    // cerr << "GetNextEntry j's RID was " << j->GetPageRID() << endl;
    int i = -1;
    if(!desc) {
      // first time in for loop ?
      i = currPos+1;

      for (; i < currNode->GetNumKeys(); i++) 
      {
        currPos = i; // save Node in object state for later.

        char* key = NULL;
        int ret = currNode->GetKey(i, (void*&)key);
        numScanned++;
        if(ret == -1) 
          return IX_PF; // TODO better error
        //std::cerr << "GetNextEntry curr entry " << *(int*)key << std::endl;

        if(pred->eval(key, pred->initOp())) {
          // std::cerr << "GetNextRec pred match for RID " << current << std::endl;
          k = key;
          rid = currNode->GetAddr(i);
          return 0;
        }
      }
    } else { // Descending
      // first time in for loop ?
      i = currPos-1;

      for (; i >= 0; i--) 
      {
        currPos = i; // save Node in object state for later.

        // std::cerr << "GetNextRec ret pos " << currPos << std::endl;
        char* key = NULL;
        int ret = currNode->GetKey(i, (void*&)key);
        numScanned++;
        if(ret == -1) 
          return IX_PF; // TODO better error
      
        if(pred->eval(key, pred->initOp())) {
          // std::cerr << "GetNextRec pred match for RID " << current << std::endl;
          k = key;
          rid = currNode->GetAddr(i);
          return 0;
        }
      }

    }
    if( (lastNode!= NULL) && 
        currNode->GetPageRID() == lastNode->GetPageRID() )
      break;
    // Advance to a new page
    if(!desc) {
      PageNum right = currNode->GetRight();
      delete currNode;
      currNode = NULL;
      currNode = pixh->FetchNode(right);
      currPos = -1;
    }
    else {
      PageNum left = currNode->GetLeft();
      delete currNode;
      currNode = NULL;
      currNode = pixh->FetchNode(left);
      if(currNode != NULL)
        currPos = currNode->GetNumKeys();
    }
  } // for j

  return IX_EOF;
}

RC IX_IndexScan::CloseScan()
{
  if(!bOpen)
    return IX_FNOTOPEN;
  assert(pixh != NULL || pred != NULL || bOpen);
  bOpen = false;
  if (pred != NULL)
    delete pred;
  pred = NULL;
  currNode = NULL;
  currPos = -1;
  lastNode = NULL;
  eof = false;
  return 0;
}

// Set up current pointers based on btree
RC IX_IndexScan::OpOptimize(CompOp     c,
                            void       *value)
{
  if(!bOpen)
    return IX_FNOTOPEN;
  
  if(value == NULL)
    return 0; //nothing to optimize

  // no opt possible
  if(c == NE_OP)
    return 0;
  RID r(-1, -1);

  if(currNode != NULL) delete currNode;
  currNode = pixh->FetchNode(pixh->FindLeaf(value)->GetPageRID().Page());
  currPos = currNode->FindKey((const void*&)value);

  // find rightmost version of a value and go left from there.
  if((c == LE_OP || c == LT_OP) && desc == true) {
    lastNode = NULL;
    currPos = currPos + 1; // go one past
  }
  
  if((c == EQ_OP) && desc == true) {
    if(currPos == -1) {// key does not exist
      delete currNode;
      eof = true;
      return 0;
    }
    // reset cause you could miss first value
    lastNode = NULL;
    currPos = currPos + 1; // go one past
  }

  // find rightmost version of value lesser than and go left from there.
  if((c == GE_OP) && desc == true) {
    lastNode = NULL;
    delete currNode;
    currNode = NULL;
    currPos = -1;
  }

  if((c == GT_OP) && desc == true) {
    lastNode = pixh->FetchNode(currNode->GetPageRID());
    delete currNode;
    currNode = NULL;
    currPos = -1;
  }


  if(desc == false) {
    if((c == LE_OP || c == LT_OP)) {
      lastNode = pixh->FetchNode(currNode->GetPageRID());
      delete currNode;
      currNode = NULL;
      currPos = -1;
    }
    if((c == GT_OP)) {
      lastNode = NULL;
      // currNode = pixh->FetchNode(currNode->GetPageRID());
      // currNode->Print(cerr);
      // cerr << "GT curr was " << currNode->GetPageRID() << endl;
    }
    if((c == GE_OP)) {
      delete currNode;
      currNode = NULL;
      currPos = -1;
      lastNode = NULL;
    }
    if((c == EQ_OP)) {
      if(currPos == -1) { // key does not exist
        delete currNode;
        eof = true;
        return 0;
      }
      lastNode = pixh->FetchNode(currNode->GetPageRID());
      delete currNode;
      currNode = NULL;
      currPos = -1;
    }
  }
  return 0;
}
